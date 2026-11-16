#!/usr/bin/env python3
"""Apply the remaining HID gamepad edits to an InkBoy checkout (run from the repository root).

Every edit anchors on exact existing text. If any anchor is missing or already replaced, nothing is
written and the script says which one, so it is safe to run once and to re-run after a failure."""
import sys
from pathlib import Path

EDITS = {}


def edit(path, old, new):
    EDITS.setdefault(path, []).append((old, new))


# --- Menu and startup -------------------------------------------------------------------------
edit('main/settings_ui.c',
'''        const char *items[] = {connection, "MAP BUTTONS"};
        int choice = menu_choose("CONTROLLER", "", items, 2, &selected, NULL);
        if (choice < 0) return;
        if (choice == 1) { input_remap(); continue; }
''',
'''        /* Offered only while Bluetooth is selected and a gamepad is paired. */
        const bool can_forget = controller_transport() == CONTROLLER_BLUETOOTH && controller_pad_paired();
        const char *items[] = {connection, "MAP BUTTONS", "FORGET GAMEPAD"};
        int choice = menu_choose("CONTROLLER", "", items, can_forget ? 3 : 2, &selected, NULL);
        if (choice < 0) return;
        if (choice == 1) { input_remap(); continue; }
        if (choice == 2) {
            if (menu_confirm("FORGET GAMEPAD", "UNPAIR IT, THEN PAIR ANOTHER", "FORGET")) controller_forget_pad();
            continue;
        }
''')

edit('main/app.c',
'''        if (play && !screen_failed() && input_wait_controller()) {
            if (!screen_close()) break;
''',
'''        if (play && !screen_failed() && input_wait_controller()) {
            /* Resume has no mapping check of its own, and a different pad may be connected now. */
            if (!input_mapping_valid() && !input_calibrate()) continue;
            if (!screen_close()) break;
''')

# --- Host tests -------------------------------------------------------------------------------
edit('tools/test_hardware_input.py', 'uint8_t controller_raw(void) {', 'uint32_t controller_raw(void) {')
edit('tools/test_hardware_input.py',
     'bool controller_connected(void) { return true; }',
     'bool controller_connected(void) { return true; }\ncontroller_profile_t controller_profile(void) { return CONTROLLER_PROFILE_INKBOY; }')

edit('tools/test_input.py',
'''int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "default")) published_wiring_is_the_default();''',
'''/* HID pads send normalized controls (bit 0..3 D-pad, bit 3+N button N) and have no standard wiring. */
static void hid_pad_needs_its_own_mapping(void)
{
    const uint8_t saved[] = {0x20, 0x10, 0x80, 0x40, 0x02, 0x04, 0x08, 0x01};
    nvs_handle_t handle = open_settings(CONTROLLER_BLUETOOTH);
    host_nvs_seed("gamepad", "buttons", HOST_NVS_BLOB, saved, sizeof(saved));
    input_load_mapping(handle);
    fake_ble_profile = BLE_GAMEPAD_HID;
    fake_ble_raw = 1u << 17;
    assert(controller_profile() == CONTROLLER_PROFILE_HID);
    assert(!input_mapping_valid() && input_buttons() == 0 && host_nvs_commits == 0); /* Not guessed, not written. */
    fake_ble_profile = BLE_GAMEPAD_INKBOY; /* The InkBoy controller keeps its own saved mapping. */
    fake_ble_raw = 0x20;
    assert(input_mapping_valid() && input_buttons() == A);
}

static void saved_hid_mapping_applies(void)
{
    /* A, B, Select, Start, Right, Left, Up, Down. */
    const uint32_t hid[8] = {1u << 17, 1u << 16, 1u << 15, 1u << 14, 1u << 3, 1u << 2, 1u << 0, 1u << 1};
    const uint32_t duplicate[8] = {1u << 17, 1u << 17, 1u << 15, 1u << 14, 1u << 3, 1u << 2, 1u << 0, 1u << 1};
    nvs_handle_t handle = open_settings(CONTROLLER_BLUETOOTH);
    host_nvs_seed("gamepad", "hid_buttons", HOST_NVS_BLOB, hid, sizeof(hid));
    input_load_mapping(handle);
    fake_ble_profile = BLE_GAMEPAD_HID;
    assert(input_mapping_valid());
    fake_ble_raw = (1u << 17) | (1u << 0) | (1u << 20); /* An unmapped button does nothing. */
    assert(input_buttons() == (A | UP));
    fake_ble_raw = (1u << 1) | (1u << 14);
    assert(input_buttons() == (DOWN | START));
    fake_ble_profile = BLE_GAMEPAD_INKBOY; /* The InkBoy controller still has the published wiring. */
    fake_ble_raw = 0x40;
    assert(input_buttons() == A);
    host_nvs_seed("gamepad", "hid_buttons", HOST_NVS_BLOB, duplicate, sizeof(duplicate));
    input_load_mapping(handle);
    fake_ble_profile = BLE_GAMEPAD_HID;
    assert(!input_mapping_valid()); /* A damaged blob is ignored rather than half used. */
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "hid")) hid_pad_needs_its_own_mapping();
    else if (!strcmp(argv[1], "hid_saved")) saved_hid_mapping_applies();
    else if (!strcmp(argv[1], "default")) published_wiring_is_the_default();''')
edit('tools/test_input.py',
     "scenarios=[[name] for name in ('default', 'saved', 'fallback', 'usb', 'wait')])",
     "scenarios=[[name] for name in ('default', 'saved', 'fallback', 'usb', 'wait', 'hid', 'hid_saved')])")

edit('tools/test_settings_ui.py',
'''    if (!strcmp(title,"CONTROLLER")) assert(count == 2 && !strncmp(items[0],"CONNECTION:",11) && !strcmp(items[1],"MAP BUTTONS"));''',
'''    if (!strcmp(title,"CONTROLLER")) assert((count == 2 || (count == 3 && !strcmp(items[2],"FORGET GAMEPAD"))) && !strncmp(items[0],"CONNECTION:",11) && !strcmp(items[1],"MAP BUTTONS"));''')
edit('tools/test_settings_ui.py',
'static void usb_failure(void) {',
'''static void forget(unsigned confirm) {
    start(); fake_ble_paired = true; fake_ble_forgets = 0;
    push("SETTINGS",1); push("CONTROLLER",2); push("FORGET GAMEPAD",(int)confirm);
    push("CONTROLLER",-1); push("SETTINGS",-1); finish();
    assert(fake_ble_forgets == confirm && fake_ble_paired == !confirm && !fake_menu_notices);
}
static void usb_failure(void) {''')
edit('tools/test_settings_ui.py',
     'else if(!strcmp(argv[1],"failures")) failures(); else usb_failure();',
     'else if(!strcmp(argv[1],"failures")) failures();\n    else if(!strcmp(argv[1],"forget")) forget(1); else if(!strcmp(argv[1],"forget-cancel")) forget(0); else usb_failure();')
edit('tools/test_settings_ui.py',
     "['groups','transports','failures','usb-failure','startup','commit','transport-failures']",
     "['groups','transports','failures','usb-failure','startup','commit','transport-failures','forget','forget-cancel']")

# --- Documentation ----------------------------------------------------------------------------
edit('README.md',
     '- Support for generic Bluetooth HID gamepads, beyond the custom InkBoy controller.',
     '- Support for generic USB HID gamepads. Generic Bluetooth gamepads already work.')
edit('README.md',
     '| Custom ESP32-C3 gamepad, connected over BLE or a USB-C data cable |',
     '| Custom ESP32-C3 gamepad over BLE or a USB-C data cable, or a Bluetooth LE HID gamepad |')
edit('README.md',
     '**Only the custom InkBoy controller is currently supported.** Generic Bluetooth and USB HID gamepads are not supported.',
     '**Over USB, only the custom InkBoy controller is supported.** Over Bluetooth it and generic Bluetooth LE HID gamepads work. '
     'Bluetooth Classic gamepads, such as PlayStation and Switch Pro controllers, cannot connect: the ESP32-S3 has no Classic Bluetooth.')
edit('README.md',
     '- **Bluetooth:** turn on the custom controller. InkBoy finds it automatically as `ESP32 Gamepad`.',
     '- **Bluetooth:** turn on the custom controller; InkBoy finds it automatically as `ESP32 Gamepad`. For another gamepad, put it in '
     'pairing mode. The first one found is paired and remembered, and nothing else connects after that. A new gamepad asks you to press '
     'each button once the first time you start a game. To pair a different one, use **Settings → Controller → Forget gamepad**.')
edit('docs/compatibility.md',
     '- Only the InkBoy controller is supported. Other Bluetooth or USB gamepads are not.',
     '- Over Bluetooth, the InkBoy controller and Bluetooth LE HID gamepads work; Bluetooth Classic gamepads cannot connect because the '
     'ESP32-S3 has no Classic Bluetooth. Over USB only the InkBoy controller is supported. A generic gamepad needs a pad that reports a '
     'gamepad or joystick HID descriptor, and only one is remembered at a time.')
edit('docs/ui-controls.md',
     'Cancellation, disconnection or storage failure keeps the prior layout.',
     'Cancellation, disconnection or storage failure keeps the prior layout. A generic Bluetooth gamepad has its own saved layout, '
     'and the wizard runs the first time you start a game with it. Forget gamepad appears in Controller settings while Bluetooth is '
     'selected and a gamepad is paired; it unpairs that gamepad so another can be paired.')
edit('docs/tuning.md',
     '| Controller | Connection (Bluetooth / USB), Map buttons, for either transport |',
     '| Controller | Connection (Bluetooth / USB), Map buttons, and Forget gamepad (Bluetooth, once a gamepad is paired) |')


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
