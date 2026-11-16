#include "input.h"
#include "diagnostics.h"
#include "controller.h"
#include "gb_emulator.h"
#include "screen.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <string.h>

static const char *TAG = "player";
/* Indexed by the Game Boy button bit: A,B,Select,Start,Right,Left,Up,Down. Each entry is one controller
 * bit. The InkBoy controller sends 8 raw bits; HID pads send the normalized controls of hid_gamepad.h,
 * so each kind keeps its own mapping (NVS "buttons" as 8 bytes, "hid_buttons" as 8 words). */
static uint32_t inkboy_mapping[8], hid_mapping[8];
static nvs_handle_t settings;
/* Set when the user chose to start a game without a controller. While set, the game does not stop for
 * the missing controller. The Home loop clears it before every menu. */
static _Atomic bool bypass;

static bool hid_active(void) { return controller_profile() == CONTROLLER_PROFILE_HID; }
static const uint32_t *active_mapping(void) { return hid_active() ? hid_mapping : inkboy_mapping; }

static int read_pin(int pin)
{
#ifdef INKBOY_DIAGNOSTICS
    int simulated = diagnostics_gpio(pin);
    if (simulated >= 0) return simulated;
#endif
    return gpio_get_level(pin);
}

uint8_t input_buttons(void)
{
    const uint32_t raw = controller_raw(), *mapping = active_mapping();
    uint8_t result = 0;
    for (unsigned i = 0; i < 8; ++i) if (raw & mapping[i]) result |= 1u << i;
    return result;
}

typedef struct { bool raw, stable, handled; int64_t changed, pressed; } button_state_t;
static button_state_t board_buttons[2];
static _Atomic bool gameplay;
static bool chord, chord_reported;
static int64_t chord_started;
static unsigned events[32];
static _Atomic unsigned event_read, event_write;

static void enqueue(unsigned event)
{
    unsigned write = atomic_load(&event_write), read = atomic_load(&event_read);
    if (write - read < 32) { events[write % 32] = event;
#ifdef INKBOY_DIAGNOSTICS
        ESP_LOGI("test", "PLAYER_BUTTON,event,%u,game,%u",event,atomic_load(&gameplay));
#endif
         atomic_store(&event_write, write + 1); }
}

unsigned input_hardware_event(void)
{
    unsigned read = atomic_load(&event_read);
    if (read == atomic_load(&event_write)) return INPUT_NONE;
    unsigned event = events[read % 32];
    atomic_store(&event_read, read + 1);
    if (event == INPUT_PC) (void)controller_toggle_usb();
    return event;
}

void input_set_gameplay(bool enabled) { atomic_store(&gameplay, enabled); }
void input_clear_events(void) { while (input_hardware_event()) {} }

void input_sample_hardware(void)
{
    int64_t now = esp_timer_get_time();
    bool down[2] = {read_pin(GPIO_NUM_0) == 0, read_pin(GPIO_NUM_18) == 0};
    if (down[0] && down[1]) {
        if (!chord) {
            if (atomic_load(&gameplay)) enqueue(INPUT_BACK);
            chord=true;chord_reported=false;
        }
        if (!chord_started) chord_started=now;
        if (!chord_reported && now-chord_started>=2000000) {
            enqueue(INPUT_PC);chord_reported=true;
        }
    } else chord_started=0;
    for (unsigned i = 0; i < 2; ++i) {
        button_state_t *b = &board_buttons[i];
        bool raw = down[i];
        if (raw != b->raw) { b->raw = raw; b->changed = now; }
        if (now - b->changed >= 30000 && b->stable != raw) {
            b->stable = raw;
            if (raw) { b->pressed = now; b->handled = chord; }
            else if (!b->handled) { enqueue(i ? INPUT_NEXT : INPUT_SELECT); b->handled = true; }
        }
        if (chord) b->handled = true;
        if (!b->stable || b->handled) continue;
        if (atomic_load(&gameplay)) {
            enqueue(i ? INPUT_NEXT : INPUT_SELECT); b->handled = true;
        } else if (now - b->pressed >= 700000) {
            enqueue(i ? INPUT_BACK : INPUT_PREVIOUS); b->handled = true;
        }
    }
    if (!down[0] && !down[1] && !board_buttons[0].stable && !board_buttons[1].stable) chord = false;
}

static void watch_buttons(void *unused)
{
    (void)unused;
    for (;;) { input_sample_hardware(); vTaskDelay(pdMS_TO_TICKS(10)); }
}

esp_err_t input_start_hardware_watch(void)
{
    const gpio_config_t buttons = {.pin_bit_mask = (1ULL << GPIO_NUM_0) | (1ULL << GPIO_NUM_18),
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    esp_err_t error = gpio_config(&buttons);
    if (error) return error;
    for (unsigned i = 0; i < 2; ++i) {
        bool down = read_pin(i ? GPIO_NUM_18 : GPIO_NUM_0) == 0;
        board_buttons[i] = (button_state_t){.raw = down, .stable = down, .handled = true};
    }
#ifdef INKBOY_DIAGNOSTICS
    diagnostics_start();
#endif
    return xTaskCreate(watch_buttons, "player_buttons", 2048, NULL, 2, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool input_connected(void) { return controller_connected() || atomic_load(&bypass); }
bool input_bypassed(void) { return atomic_load(&bypass); }
void input_set_bypass(bool on) { atomic_store(&bypass, on); }
uint32_t input_report_count(void) { return controller_reports(); }

bool input_wait_controller(void)
{
    if (controller_connected()) return true;
    const bool usb = controller_transport() == CONTROLLER_USB;
    if (!screen_message("ON / CONNECT", usb ? "PLUG IN THE USB CONTROLLER" : "TURN ON OR PAIR A GAMEPAD",
                        usb ? "CONNECTING OVER USB" : "CONNECTING AUTOMATICALLY", "HOLD PWR TO RETURN")) return false;
    while (!controller_connected()) { if ((input_hardware_event() >= INPUT_BACK)) return false;
        (void)screen_service(); vTaskDelay(pdMS_TO_TICKS(20)); }
    return true;
}

/* Game start: wait for a controller like input_wait_controller, but a long press of BOOT (GPIO0, which
 * the debouncer reports as INPUT_PREVIOUS) plays without one. Once chosen, later calls in the same game
 * start return at once. A long press of PWR still returns. */
bool input_wait_controller_or_bypass(void)
{
    if (controller_connected()) { atomic_store(&bypass, false); return true; }
    if (atomic_load(&bypass)) return true;
    const bool usb = controller_transport() == CONTROLLER_USB;
    if (!screen_message("ON / CONNECT", usb ? "PLUG IN THE USB CONTROLLER" : "TURN ON OR PAIR A GAMEPAD",
                        "HOLD BOOT TO PLAY WITHOUT ONE", "HOLD PWR TO RETURN")) return false;
    while (!controller_connected()) {
        const unsigned event = input_hardware_event();
        if (event == INPUT_PREVIOUS) { atomic_store(&bypass, true); return true; }
        if (event >= INPUT_BACK) return false;
        (void)screen_service();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return true;
}

void input_wait_release(void)
{
    while (controller_raw() || read_pin(GPIO_NUM_0) == 0 || read_pin(GPIO_NUM_18) == 0) vTaskDelay(pdMS_TO_TICKS(20));
    vTaskDelay(pdMS_TO_TICKS(40));
    input_clear_events();
}

/* One distinct controller bit per Game Boy button. The InkBoy controller uses exactly its 8 bits. */
static bool mapping_ok(const uint32_t m[8], bool exactly_eight_bits)
{
    uint32_t used = 0;
    for (unsigned i=0;i<8;++i) {
        uint32_t b = m[i];
        if (!b || (b & (b-1)) || (used & b)) return false;
        used |= b;
    }
    return !exactly_eight_bits || used == 255;
}

bool input_mapping_valid(void) { return mapping_ok(active_mapping(), !hid_active()); }

static esp_err_t save_mapping(bool hid, const uint32_t m[8])
{
    esp_err_t error;
    if (hid) error = nvs_set_blob(settings, "hid_buttons", m, 8 * sizeof(uint32_t));
    else {
        uint8_t legacy[8];
        for (unsigned i = 0; i < 8; ++i) legacy[i] = (uint8_t)m[i];
        error = nvs_set_blob(settings, "buttons", legacy, sizeof(legacy));
    }
    return error ? error : nvs_commit(settings);
}

static bool mapping_release(void)
{
    while (controller_raw() || read_pin(GPIO_NUM_0) == 0 || read_pin(GPIO_NUM_18) == 0) {
        if (!controller_connected() || (input_hardware_event() >= INPUT_BACK)) return false;
        if (screen_service() < 0) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

bool input_calibrate(void)
{
    static const char *names[] = {"PRESS UP", "PRESS DOWN", "PRESS LEFT", "PRESS RIGHT",
                                  "PRESS A", "PRESS B", "PRESS START", "PRESS SELECT"};
    static const uint8_t order[] = {6,7,5,4,0,1,3,2};
    if (!input_wait_controller()) return false;
    const bool hid = hid_active(); /* The kind of controller being mapped; it must stay the same throughout. */
    uint32_t used = 0;
    uint32_t candidate[8] = {0};
    for (unsigned step=0;step<8;++step) {
        if (!input_wait_controller() || hid_active() != hid) return false;
        if (!mapping_release()) return false;
        if (!screen_prompt(names[step], "PRESS ONE CONTROLLER BUTTON")) return false;
        ESP_LOGI(TAG, "MAP_WAIT,step,%u,control,%s", step, names[step]);
        for (;;) {
            if (!controller_connected() || hid_active() != hid || (input_hardware_event() >= INPUT_BACK)) return false;
            if (screen_service() < 0) return false;
            uint32_t raw = controller_raw();
            if (raw && !(raw & (raw-1)) && !(raw & used)) {
                candidate[order[step]] = raw;
                used |= raw;
                ESP_LOGI(TAG, "MAP_BUTTON,control,%s,raw,%02x", names[step], (unsigned)raw);
                if (!mapping_release()) return false;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    uint32_t *current = hid ? hid_mapping : inkboy_mapping;
    esp_err_t error = save_mapping(hid, candidate);
    if (error) {
        (void)save_mapping(hid, current);
        screen_message("SETUP ERROR", "CANNOT SAVE BUTTON MAPPING", "PREVIOUS MAPPING KEPT", "");
        return false;
    }
    memcpy(current, candidate, sizeof(candidate));
    ESP_LOGI(TAG, "MAP_SAVED");
    return true;
}

void input_remap(void)
{
    uint32_t old_inkboy[8], old_hid[8];
    memcpy(old_inkboy, inkboy_mapping, sizeof(old_inkboy));
    memcpy(old_hid, hid_mapping, sizeof(old_hid));
    if (!input_calibrate()) {
        memcpy(inkboy_mapping, old_inkboy, sizeof(old_inkboy));
        memcpy(hid_mapping, old_hid, sizeof(old_hid));
    }
}

void input_load_mapping(nvs_handle_t nvs)
{
    settings = nvs;
    uint8_t legacy[8];
    size_t legacy_size = sizeof(legacy);
    memset(inkboy_mapping, 0, sizeof(inkboy_mapping));
    if (nvs_get_blob(settings, "buttons", legacy, &legacy_size) == ESP_OK && legacy_size == sizeof(legacy))
        for (unsigned i = 0; i < 8; ++i) inkboy_mapping[i] = legacy[i];
    if (!mapping_ok(inkboy_mapping, true)) {
        /* Down=GPIO0, Left=1, Up=2, Right=3, Select=4, B=5, A=6, Start=7 on the
         * controller. A saved mapping wins and nothing is written here. */
        const uint8_t published_wiring[] = {0x40,0x20,0x10,0x80,0x08,0x02,0x04,0x01};
        for (unsigned i = 0; i < 8; ++i) inkboy_mapping[i] = published_wiring[i];
        ESP_LOGI(TAG, "PLAYER_MAPPING: using the published controller wiring");
    }
    /* HID pads have no standard wiring: without a saved mapping this stays empty, which is invalid
     * and makes the first game start run the mapping wizard. */
    uint32_t stored[8];
    size_t stored_size = sizeof(stored);
    memset(hid_mapping, 0, sizeof(hid_mapping));
    if (nvs_get_blob(settings, "hid_buttons", stored, &stored_size) == ESP_OK && stored_size == sizeof(stored) &&
        mapping_ok(stored, false)) memcpy(hid_mapping, stored, sizeof(stored));
}
