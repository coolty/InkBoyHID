#include "ble_gamepad.h"
#include "hid_gamepad.h"
#include "esp_hidh.h"
#include "esp_hid_gap.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "gamepad";
static const char INKBOY_NAME[] = "ESP32 Gamepad";
static _Atomic bool connected, stopping, task_running, close_requested, opening_inkboy;
static _Atomic uint32_t raw_buttons, last_report_ms, reports;
static _Atomic uint8_t profile;
static _Atomic(esp_hidh_dev_t *) device;
static bool stack_ready;
/* Written and read by the esp_hidh event task only. */
static hid_pad_t pad;
static uint16_t pad_map;
/* Written by the connect task only: the device being opened and ones that turned out not to be gamepads. */
static uint8_t opening[6], rejected[4][6];
static unsigned rejected_next;
/* The paired HID pad. Read and written by the connect task only, after ble_gamepad_load() at startup.
 * Layout is the NVS blob "pad_saved": 40 bytes, no padding. */
typedef struct { uint8_t version, type, bda[6]; char name[32]; } saved_pad_t;
static nvs_handle_t settings;
static saved_pad_t saved;
static bool have_saved;
static uint8_t opening_type;
static char opening_name[32];
/* A forgotten pad is ignored for a while, or it would reconnect before its owner turns it off. */
static uint8_t holdoff_bda[6];
static uint32_t holdoff_until_ms;
static _Atomic bool save_pending, forget_requested, paired;
#define HOLDOFF_MS 60000u
/* Pairing scan results, filled by the connect task and read by the menu once scan_finished is set. */
typedef struct { char name[24]; int8_t rssi; uint8_t type, bda[6]; } found_t;
static found_t found[BLE_GAMEPAD_FOUND_MAX];
static unsigned found_count;
static _Atomic bool scan_requested, scan_finished;
static _Atomic int pair_index = -1;

enum { STOP_WAIT_MS = 10000, CLOSE_WAIT_MS = 2000, POLL_MS = 20 };

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time()/1000); }

/* Decides what the opened device is. The InkBoy controller keeps its fixed report; anything else
 * needs a report descriptor with a gamepad collection. */
static bool adopt(esp_hidh_dev_t *dev)
{
    if (atomic_load(&opening_inkboy)) { atomic_store(&profile, BLE_GAMEPAD_INKBOY); return true; }
    size_t count = 0;
    esp_hid_raw_report_map_t *maps = NULL;
    esp_err_t error = esp_hidh_dev_report_maps_get(dev, &count, &maps);
    if (error != ESP_OK) { ESP_LOGW(TAG, "PAD_NO_REPORT_MAP,%s", esp_err_to_name(error)); return false; }
    for (size_t i = 0; i < count; ++i) {
        if (!hid_pad_parse(maps[i].data, maps[i].len, &pad)) continue;
        pad_map = (uint16_t)i;
        atomic_store(&profile, BLE_GAMEPAD_HID);
        ESP_LOGI(TAG, "PAD_HID,map,%u,controls,%08x", (unsigned)i, (unsigned)pad.controls);
        return true;
    }
    ESP_LOGW(TAG, "PAD_NOT_A_GAMEPAD,maps,%u", (unsigned)count);
    return false;
}

static void hid_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    esp_hidh_event_data_t *event = data;
    switch (id) {
    case ESP_HIDH_OPEN_EVENT: {
        atomic_store(&raw_buttons, 0);
        atomic_store(&last_report_ms, now_ms());
        const bool ok = event->open.status == ESP_OK && adopt(event->open.dev);
        atomic_store(&connected, ok);
        if (ok && atomic_load(&profile) == BLE_GAMEPAD_HID) atomic_store(&save_pending, true);
        if (event->open.status == ESP_OK && !ok) atomic_store(&close_requested, true);
        ESP_LOGI(TAG, "PAD_OPEN,status,%d,name,%s,profile,%u", event->open.status,
                 event->open.status == ESP_OK ? esp_hidh_dev_name_get(event->open.dev) : "failed",
                 (unsigned)atomic_load(&profile));
        if (event->open.status == ESP_OK) esp_hidh_dev_dump(event->open.dev, stdout);
        break;
    }
    case ESP_HIDH_INPUT_EVENT:
        if (atomic_load(&profile) == BLE_GAMEPAD_HID) {
            bool known;
            if (event->input.map_index != pad_map) break;
            const uint32_t controls = hid_pad_decode(&pad, event->input.report_id, event->input.data,
                                                     event->input.length, &known);
            if (!known) break;
            const uint32_t previous = atomic_exchange(&raw_buttons, controls);
            atomic_store(&last_report_ms, now_ms());
            atomic_fetch_add(&reports, 1);
            if (previous != controls) ESP_LOGI(TAG, "PAD_CONTROLS,%08x", (unsigned)controls);
        } else if (event->input.report_id == 3 && event->input.length >= 2) {
            /* ESP32-BLE-Gamepad 0.7.4: report 3 begins with the little-endian
             * button bitmap. esp_hidh reports ID separately, outside data[]. */
            uint8_t buttons = event->input.data[0];
            uint32_t previous = atomic_exchange(&raw_buttons, buttons);
            atomic_store(&last_report_ms, now_ms());
            atomic_fetch_add(&reports, 1);
            if (previous != buttons) ESP_LOGI(TAG, "PAD_BUTTONS,%02x", buttons);
        }
        break;
    case ESP_HIDH_CLOSE_EVENT:
        atomic_store(&raw_buttons, 0);
        atomic_store(&connected, false);
        atomic_store(&close_requested, false);
        atomic_store(&device, NULL);
        ESP_LOGI(TAG, "PAD_DISCONNECTED: released all buttons; rescanning");
        break;
    default: break;
    }
}

static bool is_rejected(const uint8_t *bda)
{
    for (unsigned i = 0; i < 4; ++i) if (!memcmp(rejected[i], bda, 6)) return true;
    return false;
}

/* The InkBoy controller wins; otherwise the paired pad, if it is in range. */
static const esp_hid_scan_result_t *choose(const esp_hid_scan_result_t *results, bool *inkboy)
{
    const esp_hid_scan_result_t *best = NULL;
    *inkboy = false;
    for (const esp_hid_scan_result_t *r = results; r && !atomic_load(&stopping); r = r->next) {
        if (r->transport != ESP_HID_TRANSPORT_BLE) continue;
        if (r->name && !strcmp(r->name, INKBOY_NAME)) { *inkboy = true; return r; }
        ESP_LOGI(TAG, "PAD_SEEN,name,%s,usage,%d,rssi,%d", r->name ? r->name : "?", (int)r->usage, r->rssi);
        if (is_rejected(r->bda)) continue;
        if ((int32_t)(holdoff_until_ms - now_ms()) > 0 && !memcmp(r->bda, holdoff_bda, 6)) continue;
        /* Only the paired pad reconnects by itself; new ones are paired from the menu. A name match
         * covers pads that change their address. */
        if (!have_saved || (memcmp(r->bda, saved.bda, 6) && (!r->name || strcmp(r->name, saved.name)))) continue;
        if ((r->usage == ESP_HID_USAGE_GAMEPAD || r->usage == ESP_HID_USAGE_JOYSTICK) && (!best || r->rssi > best->rssi)) best = r;
    }
    return best;
}

static bool holdoff_active_for_opening(void)
{
    return (int32_t)(holdoff_until_ms - now_ms()) > 0 && !memcmp(opening, holdoff_bda, 6);
}

static void remember(void)
{
    saved_pad_t next = {.version = 1, .type = opening_type};
    memcpy(next.bda, opening, sizeof(next.bda));
    snprintf(next.name, sizeof(next.name), "%s", opening_name);
    if (have_saved && !memcmp(&saved, &next, sizeof(next))) return;
    if (have_saved && memcmp(saved.bda, next.bda, sizeof(next.bda))) (void)esp_ble_remove_bond_device(saved.bda);
    if (!settings || nvs_set_blob(settings, "pad_saved", &next, sizeof(next)) != ESP_OK || nvs_commit(settings) != ESP_OK) {
        ESP_LOGW(TAG, "PAD_SAVE_FAILED");
        return;
    }
    saved = next;
    have_saved = true;
    atomic_store(&paired, true);
    ESP_LOGI(TAG, "PAD_PAIRED,name,%s", saved.name);
}

static void forget_now(void)
{
    esp_hidh_dev_t *opened = atomic_load(&device);
    if (opened && atomic_load(&profile) == BLE_GAMEPAD_HID) (void)esp_hidh_dev_close(opened);
    if (have_saved) {
        memcpy(holdoff_bda, saved.bda, sizeof(holdoff_bda));
        holdoff_until_ms = now_ms() + HOLDOFF_MS;
        (void)esp_ble_remove_bond_device(saved.bda);
    }
    if (settings) { (void)nvs_erase_key(settings, "pad_saved"); (void)nvs_commit(settings); }
    memset(&saved, 0, sizeof(saved));
    have_saved = false;
    atomic_store(&paired, false);
    ESP_LOGI(TAG, "PAD_FORGOTTEN");
}

/* A five second scan for the menu: every HID device that is not a keyboard or mouse, strongest first. */
static void pairing_scan(void)
{
    size_t count = 0;
    esp_hid_scan_result_t *results = NULL;
    unsigned n = 0;
    ESP_LOGI(TAG, "PAD_PAIR_SCAN");
    if (esp_hid_scan(5, &count, &results) == ESP_OK) {
        for (const esp_hid_scan_result_t *r = results; r; r = r->next) {
            ESP_LOGI(TAG, "PAD_SEEN,name,%s,usage,%d,rssi,%d", r->name ? r->name : "?", (int)r->usage, r->rssi);
            if (r->transport != ESP_HID_TRANSPORT_BLE || r->usage == ESP_HID_USAGE_KEYBOARD || r->usage == ESP_HID_USAGE_MOUSE) continue;
            found_t entry = {.rssi = r->rssi, .type = (uint8_t)r->ble.addr_type};
            memcpy(entry.bda, r->bda, sizeof(entry.bda));
            snprintf(entry.name, sizeof(entry.name), "%s", r->name && r->name[0] ? r->name : "UNKNOWN");
            unsigned at = n;
            while (at > 0 && found[at - 1].rssi < entry.rssi) --at;
            if (at >= BLE_GAMEPAD_FOUND_MAX) continue;
            const unsigned last = n < BLE_GAMEPAD_FOUND_MAX ? n : BLE_GAMEPAD_FOUND_MAX - 1;
            memmove(&found[at + 1], &found[at], (last - at) * sizeof(found[0]));
            found[at] = entry;
            if (n < BLE_GAMEPAD_FOUND_MAX) ++n;
        }
    }
    esp_hid_scan_results_free(results);
    found_count = n;
    atomic_store(&scan_finished, true);
}

/* Connects to a scan result the user picked. It becomes the paired pad once its descriptor is read. */
static void pair_now(unsigned index)
{
    if (index >= found_count) return;
    const found_t *f = &found[index];
    esp_hidh_dev_t *current = atomic_load(&device);
    if (current) (void)esp_hidh_dev_close(current);
    for (unsigned waited = 0; atomic_load(&device) && waited < 1500; waited += 20) vTaskDelay(pdMS_TO_TICKS(20));
    memcpy(opening, f->bda, sizeof(opening));
    opening_type = f->type;
    snprintf(opening_name, sizeof(opening_name), "%s", f->name);
    atomic_store(&opening_inkboy, false);
    ESP_LOGI(TAG, "PAD_PAIR,name,%s,rssi,%d", f->name, f->rssi);
    esp_hidh_dev_t *opened = esp_hidh_dev_open(opening, ESP_HID_TRANSPORT_BLE, opening_type);
    if (opened) atomic_store(&device, opened);
    else ESP_LOGW(TAG, "PAD_PAIR_FAILED");
}

static void connect_task(void *unused)
{
    (void)unused;
    while (!atomic_load(&stopping)) {
        if (!atomic_load(&connected)) {
            size_t count = 0;
            esp_hid_scan_result_t *results = NULL;
            ESP_LOGI(TAG, "PAD_SCAN: looking for a gamepad");
            esp_err_t error = esp_hid_scan(3, &count, &results);
            if (error == ESP_OK) {
                bool inkboy;
                const esp_hid_scan_result_t *r = choose(results, &inkboy);
                if (r) {
                    ESP_LOGI(TAG, "PAD_CONNECT,rssi,%d,name,%s,inkboy,%u", r->rssi, r->name ? r->name : "?", inkboy);
                    memcpy(opening, r->bda, sizeof(opening));
                    opening_type = (uint8_t)r->ble.addr_type;
                    snprintf(opening_name, sizeof(opening_name), "%s", r->name ? r->name : "");
                    atomic_store(&opening_inkboy, inkboy);
                    /* esp_hidh's BLE open waits for discovery completion. */
                    esp_hidh_dev_t *opened = esp_hidh_dev_open(opening, r->transport, r->ble.addr_type);
                    if (opened) atomic_store(&device, opened);
                    else ESP_LOGW(TAG, "PAD_CONNECT_FAILED; retrying");
                }
            } else ESP_LOGW(TAG, "PAD_SCAN_FAILED,%s", esp_err_to_name(error));
            esp_hid_scan_results_free(results);
        }
        for (unsigned waited = 0; waited < 2000 && !atomic_load(&stopping); waited += 100) {
            if (atomic_exchange(&forget_requested, false)) forget_now();
            if (atomic_exchange(&scan_requested, false)) pairing_scan();
            const int pick = atomic_exchange(&pair_index, -1);
            if (pick >= 0) pair_now((unsigned)pick);
            if (atomic_exchange(&save_pending, false) && !atomic_load(&opening_inkboy) && !holdoff_active_for_opening()) remember();
            if (atomic_exchange(&close_requested, false)) {
                /* Not a gamepad: remember it so scans skip it, then let go. */
                memcpy(rejected[rejected_next++ % 4], opening, sizeof(opening));
                esp_hidh_dev_t *opened = atomic_load(&device);
                if (opened) (void)esp_hidh_dev_close(opened);
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    atomic_store(&task_running, false);
    vTaskDelete(NULL);
}

static esp_err_t init_stack(void)
{
    esp_err_t error = esp_hid_gap_init(HIDH_BLE_MODE);
    if (error != ESP_OK) return error;
    error = esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler);
    if (error != ESP_OK) return error;
    esp_ble_auth_req_t auth = ESP_LE_AUTH_BOND;
    esp_ble_io_cap_t io = ESP_IO_CAP_NONE;
    error = esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth, sizeof(auth));
    if (error != ESP_OK) return error;
    error = esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &io, sizeof(io));
    if (error != ESP_OK) return error;
    const esp_hidh_config_t config = {
        .callback = hid_event, .event_stack_size = 4096, .callback_arg = NULL,
    };
    return esp_hidh_init(&config);
}

esp_err_t ble_gamepad_start(void)
{
    if (!stack_ready) {
        esp_err_t error = init_stack();
        if (error != ESP_OK) return error;
        stack_ready = true;
    }
    atomic_store(&stopping, false);
    if (atomic_exchange(&task_running, true)) return ESP_OK;
    if (xTaskCreatePinnedToCore(connect_task, "pad_connect", 6144, NULL, 2, NULL, 0) == pdPASS) return ESP_OK;
    atomic_store(&task_running, false);
    return ESP_ERR_NO_MEM;
}

static bool wait_for(bool (*done)(void), unsigned timeout_ms)
{
    for (unsigned waited = 0; !done(); waited += POLL_MS) {
        if (waited >= timeout_ms) return false;
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
    return true;
}
static bool task_stopped(void) { return !atomic_load(&task_running); }
static bool device_closed(void) { return atomic_load(&device) == NULL; }

/* The Bluetooth stack and esp_hidh stay initialised: deinitialising them
 * is not needed to stop scanning or release the controller, and restarting is cheap. */
esp_err_t ble_gamepad_stop(void)
{
    atomic_store(&stopping, true);
    if (!wait_for(task_stopped, STOP_WAIT_MS)) return ESP_ERR_TIMEOUT;
    esp_hidh_dev_t *opened = atomic_load(&device);
    if (opened) {
        if (esp_hidh_dev_close(opened) != ESP_OK) atomic_store(&device, NULL);
        else if (!wait_for(device_closed, CLOSE_WAIT_MS)) return ESP_ERR_TIMEOUT;
    }
    atomic_store(&raw_buttons, 0);
    atomic_store(&connected, false);
    return ESP_OK;
}

ble_gamepad_profile_t ble_gamepad_profile(void) { return (ble_gamepad_profile_t)atomic_load(&profile); }

bool ble_gamepad_connected(void)
{
    if (!atomic_load(&connected)) return false;
    /* HID pads send reports only when something changes, so an idle pad is judged by its link, which
     * the Bluetooth supervision timeout and the close event already track. */
    if (atomic_load(&profile) == BLE_GAMEPAD_HID) return true;
    return atomic_load(&reports) > 0 && now_ms() - atomic_load(&last_report_ms) < 2000;
}
uint32_t ble_gamepad_raw(void) { return ble_gamepad_connected() ? atomic_load(&raw_buttons) : 0; }
uint32_t ble_gamepad_reports(void) { return atomic_load(&reports); }

void ble_gamepad_load(nvs_handle_t nvs)
{
    settings = nvs;
    saved_pad_t stored;
    size_t size = sizeof(stored);
    have_saved = nvs_get_blob(nvs, "pad_saved", &stored, &size) == ESP_OK && size == sizeof(stored) && stored.version == 1;
    if (have_saved) { saved = stored; saved.name[sizeof(saved.name) - 1] = 0; }
    atomic_store(&paired, have_saved);
}

bool ble_gamepad_paired(void) { return atomic_load(&paired); }
void ble_gamepad_forget(void) { atomic_store(&forget_requested, true); }

void ble_gamepad_scan_begin(void)
{
    atomic_store(&scan_finished, false);
    atomic_store(&scan_requested, true);
}

bool ble_gamepad_scan_done(unsigned *count)
{
    if (!atomic_load(&scan_finished)) return false;
    *count = found_count;
    return true;
}

const char *ble_gamepad_scan_name(unsigned index) { return index < found_count ? found[index].name : ""; }
int ble_gamepad_scan_rssi(unsigned index) { return index < found_count ? found[index].rssi : 0; }

void ble_gamepad_pair(unsigned index)
{
    atomic_store(&paired, false); /* The menu waits for this to become true again. */
    atomic_store(&pair_index, (int)index);
}
