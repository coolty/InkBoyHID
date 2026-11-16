#!/usr/bin/env python3
"""Second round of HID edits: the "Pair gamepad" menu and a wider Bluetooth scan filter.

Run from the repository root AFTER tools/apply_hid_edits.py. Every edit anchors on exact text; if any
anchor is missing or already applied, nothing is written and the script names it."""
import sys
from pathlib import Path

EDITS = {}


def edit(path, old, new):
    EDITS.setdefault(path, []).append((old, new))


# --- Controller menu: PAIR GAMEPAD scans with the board buttons and lists what it finds -------
edit('main/settings_ui.c', '#include "screen.h"\n#include "esp_log.h"',
     '#include "screen.h"\n#include "esp_log.h"\n#include "freertos/FreeRTOS.h"\n#include "freertos/task.h"')

edit('main/settings_ui.c', 'static void controller_settings(void)\n{',
'''/* Scans for Bluetooth LE gamepads and pairs the one picked from the list. The board buttons drive it:
 * PWR next, BOOT select, hold PWR to cancel. The first game start after pairing runs the mapping wizard. */
static void pair_gamepad(void)
{
    if (!screen_message("SCANNING", "LOOKING FOR GAMEPADS", "PUT YOURS IN PAIRING MODE", "HOLD PWR TO CANCEL")) return;
    input_clear_events();
    controller_scan_begin();
    unsigned count = 0;
    while (!controller_scan_done(&count)) {
        if (input_hardware_event() >= INPUT_BACK) return;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!count) {
        menu_notice("NONE FOUND", "PUT IT IN PAIRING MODE AND RETRY", "BLUETOOTH LE GAMEPADS ONLY");
        return;
    }
    if (count > CONTROLLER_SCAN_MAX) count = CONTROLLER_SCAN_MAX;
    char labels[CONTROLLER_SCAN_MAX][32];
    const char *items[CONTROLLER_SCAN_MAX + 1];
    for (unsigned i = 0; i < count; ++i) {
        snprintf(labels[i], sizeof labels[i], "%.19s  %d", controller_scan_name(i), controller_scan_rssi(i));
        items[i] = labels[i];
    }
    items[count] = MENU_BACK;
    unsigned selected = 0;
    int choice = menu_choose("PAIR GAMEPAD", "", items, count + 1, &selected, NULL);
    if (choice < 0 || (unsigned)choice == count) return;
    controller_scan_pair((unsigned)choice);
    if (!screen_message("CONNECTING", "PAIRING THE GAMEPAD", "CONFIRM ON IT IF ASKED", "HOLD PWR TO CANCEL")) return;
    input_clear_events();
    for (unsigned waited = 0; waited < 20000 && !controller_pad_paired(); waited += 50) {
        if (input_hardware_event() >= INPUT_BACK) return;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (controller_pad_paired()) menu_notice("GAMEPAD PAIRED", "IT RECONNECTS BY ITSELF", "MAP ITS BUTTONS ON FIRST GAME");
    else menu_notice("PAIRING FAILED", "TRY AGAIN IN PAIRING MODE", "BLUETOOTH LE GAMEPADS ONLY");
}

static void controller_settings(void)
{''')

edit('main/settings_ui.c',
'''        /* Offered only while Bluetooth is selected and a gamepad is paired. */
        const bool can_forget = controller_transport() == CONTROLLER_BLUETOOTH && controller_pad_paired();
        const char *items[] = {connection, "MAP BUTTONS", "FORGET GAMEPAD"};
        int choice = menu_choose("CONTROLLER", "", items, can_forget ? 3 : 2, &selected, NULL);
        if (choice < 0) return;
        if (choice == 1) { input_remap(); continue; }
        if (choice == 2) {
''',
'''        /* Pairing rows exist only while Bluetooth is selected; Forget only once a gamepad is paired. */
        const bool bluetooth = controller_transport() == CONTROLLER_BLUETOOTH;
        const bool can_forget = bluetooth && controller_pad_paired();
        const char *items[] = {connection, "MAP BUTTONS", "PAIR GAMEPAD", "FORGET GAMEPAD"};
        int choice = menu_choose("CONTROLLER", "", items, !bluetooth ? 2 : can_forget ? 4 : 3, &selected, NULL);
        if (choice < 0) return;
        if (choice == 1) { input_remap(); continue; }
        if (choice == 2) { pair_gamepad(); continue; }
        if (choice == 3) {
''')

# --- Host tests ------------------------------------------------------------------------------
edit('tools/test_settings_ui.py',
     '''assert((count == 2 || (count == 3 && !strcmp(items[2],"FORGET GAMEPAD"))) && !strncmp(items[0],"CONNECTION:",11) && !strcmp(items[1],"MAP BUTTONS"));''',
     '''assert(count >= 2 && count <= 4 && !strncmp(items[0],"CONNECTION:",11) && !strcmp(items[1],"MAP BUTTONS") && (count < 3 || !strcmp(items[2],"PAIR GAMEPAD")) && (count < 4 || !strcmp(items[3],"FORGET GAMEPAD")));''')
edit('tools/test_settings_ui.py',
     'push("CONTROLLER",2); push("FORGET GAMEPAD",(int)confirm);',
     'push("CONTROLLER",3); push("FORGET GAMEPAD",(int)confirm);')
edit('tools/test_settings_ui.py', 'static void usb_failure(void) {',
'''static void pair(unsigned found) {
    start(); fake_ble_found_count = found;
    push("SETTINGS",1); push("CONTROLLER",2);
    if (found) push("PAIR GAMEPAD",1);
    push("CONTROLLER",-1); push("SETTINGS",-1); finish();
    /* A pick pairs the second row (the list is strongest first) and shows one result notice. */
    assert(fake_ble_pair_index == (found ? 1 : -1) && fake_ble_paired == (found != 0) && fake_menu_notices == 1);
}
static void usb_failure(void) {''')
edit('tools/test_settings_ui.py',
     'else if(!strcmp(argv[1],"forget-cancel")) forget(0); else usb_failure();',
     'else if(!strcmp(argv[1],"forget-cancel")) forget(0);\n    else if(!strcmp(argv[1],"pair")) pair(2); else if(!strcmp(argv[1],"pair-none")) pair(0); else usb_failure();')
edit('tools/test_settings_ui.py', "'forget','forget-cancel']", "'forget','forget-cancel','pair','pair-none']")

# --- Scan filter: pads list the HID service anywhere in either UUID list -----------------------
edit('components/ble_gamepad/vendor/esp_hid_gap.c',
'''    if (uuid_d != NULL && uuid_len) {
        uuid = uuid_d[0] + (uuid_d[1] << 8);
    }''',
'''    /* InkBoy: the stock example only reads the first UUID of the complete list. Gamepads often list
     * the HID service after the battery service, or in the partial list, so check both lists fully. */
    for (unsigned list = 0; list < 2 && uuid != ESP_GATT_UUID_HID_SVC; ++list) {
        if (list) uuid_d = esp_ble_resolve_adv_data_by_type(scan_rst->ble_adv,
                                               scan_rst->adv_data_len + scan_rst->scan_rsp_len,
                                               ESP_BLE_AD_TYPE_16SRV_PART,
                                               &uuid_len);
        for (unsigned i = 0; uuid_d != NULL && i + 1 < uuid_len; i += 2)
            if ((uuid_d[i] | (uuid_d[i + 1] << 8)) == ESP_GATT_UUID_HID_SVC) uuid = ESP_GATT_UUID_HID_SVC;
    }''')

# --- Documentation ----------------------------------------------------------------------------
edit('README.md',
     '- **Bluetooth:** turn on the custom controller; InkBoy finds it automatically as `ESP32 Gamepad`. For another gamepad, put it in '
     'pairing mode. The first one found is paired and remembered, and nothing else connects after that. A new gamepad asks you to press '
     'each button once the first time you start a game. To pair a different one, use **Settings → Controller → Forget gamepad**.',
     '- **Bluetooth:** turn on the custom controller; InkBoy finds it automatically as `ESP32 Gamepad`. For another gamepad, open '
     '**Settings → Controller → Pair gamepad**, put the gamepad in pairing mode and pick it from the list with the board buttons '
     '(PWR next, BOOT select). It is remembered and reconnects by itself, and nothing else connects. The first time you start a game '
     'with it you press each button once. **Forget gamepad** removes it.')
edit('docs/ui-controls.md',
     'Forget gamepad appears in Controller settings while Bluetooth is '
     'selected and a gamepad is paired; it unpairs that gamepad so another can be paired.',
     'With Bluetooth selected, Controller settings has Pair gamepad, which scans for five seconds and lists the Bluetooth LE '
     'gamepads it finds (strongest first, signal in dB); choose one with the board buttons. Forget gamepad appears once one is paired '
     'and unpairs it.')


def main():
    root = Path.cwd()
    results, problems = {}, []
    for path, edits in EDITS.items():
        file = root / path
        if not file.exists():
            problems.append(f'{path}: file not found (run from the repository root)')
            continue
        text = file.read_text()
        for old, new in edits:
            first = old.splitlines()[0][:70]
            if new in text:
                problems.append(f'{path}: already applied: {first!r}')
            elif text.count(old) != 1:
                problems.append(f'{path}: expected exactly one match, found {text.count(old)}: {first!r}')
            else:
                text = text.replace(old, new)
        results[path] = text
    if problems:
        print('Nothing was changed:\n  ' + '\n  '.join(problems))
        return 1
    for path, text in results.items():
        (root / path).write_text(text)
        print(f'updated {path}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
