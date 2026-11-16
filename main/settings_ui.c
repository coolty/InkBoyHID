#include "settings_ui.h"
#include "controller.h"
#include "display_config.h"
#include "display_preset.h"
#include "fast_display.h"
#include "game_audio.h"
#include "gray_settings_ui.h"
#include "input.h"
#include "menu.h"
#include "preset_ui.h"
#include "screen.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "player";
static nvs_handle_t settings;
static bool sound_enabled = true;
/* 1..10 in 3 dB steps; 6 is the original fixed -12 dB, 10 is unity gain. */
#define VOLUME_MAX 10
#define VOLUME_DEFAULT 6
static uint8_t volume = VOLUME_DEFAULT;
static float volume_db(unsigned level) { return 3.0f * (float)level - 30.0f; }
/* Minutes of play before the random carousel switches to another game. */
static const uint8_t carousel_steps[] = {1, 2, 3, 5, 10, 15, 20, 30, 45, 60};
#define CAROUSEL_STEPS (sizeof(carousel_steps) / sizeof(carousel_steps[0]))
#define CAROUSEL_DEFAULT_STEP 4 /* 10 minutes */
static bool carousel_minutes_valid(unsigned minutes)
{
    for (unsigned i = 0; i < CAROUSEL_STEPS; ++i) if (carousel_steps[i] == minutes) return true;
    return false;
}
static unsigned carousel_index(unsigned minutes)
{
    for (unsigned i = 0; i < CAROUSEL_STEPS; ++i) if (carousel_steps[i] == minutes) return i;
    return CAROUSEL_DEFAULT_STEP;
}

void settings_ui_init(nvs_handle_t nvs) { settings = nvs; }
bool settings_ui_sound_enabled(void) { return sound_enabled; }

void settings_ui_load_display(void)
{
    display_config_t config = display_config_get();
    uint8_t stored_mode;
    if (nvs_get_u8(settings, "display_mode", &stored_mode) == ESP_OK && stored_mode < DISPLAY_MODE_COUNT)
        config.mode = (display_mode_t)stored_mode;
    const display_preset_t *dithered = display_preset_active(DISPLAY_DITHERED);
    config.dither_black_pulses = dithered->black_pulses;
    config.dither_white_pulses = dithered->white_pulses;
    display_config_set(&config);
    ESP_LOGI(TAG, "PLAYER_DITHER_SETTINGS,black,%u,white,%u", config.dither_black_pulses, config.dither_white_pulses);
    ESP_LOGI(TAG, "PLAYER_SETTINGS,mode,%u", config.mode);
}

void settings_ui_load_toggles(void)
{
    display_config_t config = display_config_get();
    uint8_t stored_fps=0;
    if (nvs_get_u8(settings,"show_fps",&stored_fps)==ESP_OK && stored_fps<=1)
        config.show_fps=stored_fps;
    display_config_set(&config);
    ESP_LOGI(TAG,"PLAYER_FPS,enabled,%u",config.show_fps);
    uint8_t stored_border;
    if (nvs_get_u8(settings, "border", &stored_border) == ESP_OK && stored_border < BORDER_COUNT)
        config.border = (game_border_t)stored_border;
    display_config_set(&config);
    ESP_LOGI(TAG, "PLAYER_BORDER,border,%u", config.border);
    uint8_t stored_rotation;
    if (nvs_get_u8(settings, "rotation", &stored_rotation) == ESP_OK && stored_rotation < SCREEN_ROTATE_COUNT)
        config.rotation = (screen_rotation_t)stored_rotation;
    uint8_t stored_carousel, stored_minutes;
    if (nvs_get_u8(settings, "carousel", &stored_carousel) == ESP_OK && stored_carousel <= 1)
        config.carousel = stored_carousel;
    if (nvs_get_u8(settings, "carousel_min", &stored_minutes) == ESP_OK && carousel_minutes_valid(stored_minutes))
        config.carousel_minutes = stored_minutes;
    display_config_set(&config);
    ESP_LOGI(TAG, "PLAYER_ROTATION,rotation,%u", config.rotation);
    ESP_LOGI(TAG, "PLAYER_CAROUSEL_SETTINGS,enabled,%u,minutes,%u", config.carousel, config.carousel_minutes);
    uint8_t stored_sound = 1;
    if (nvs_get_u8(settings, "sound", &stored_sound) == ESP_OK && stored_sound <= 1)
        sound_enabled = stored_sound;
    uint8_t stored_volume;
    if (nvs_get_u8(settings, "volume", &stored_volume) == ESP_OK && stored_volume >= 1 && stored_volume <= VOLUME_MAX)
        volume = stored_volume;
    game_audio_set_volume(volume_db(volume));
    ESP_LOGI(TAG, "PLAYER_VOLUME,level,%u", volume);
}


static esp_err_t save_value(const char *key, uint8_t next, uint8_t previous)
{
    esp_err_t error = nvs_set_u8(settings, key, next);
    if (error) return error;
    error = nvs_commit(settings);
    if (error) {
        /* Prevent a later successful commit from applying this failed edit. */
        (void)nvs_set_u8(settings, key, previous);
        (void)nvs_commit(settings);
    }
    return error;
}

static void change_mode(void)
{
    const char *items[] = {"FAST DITHERED", "FAST GRAYSCALE"};
    display_config_t next = display_config_get(); unsigned selected = next.mode;
    int choice = menu_choose("DISPLAY MODE", "", items, 2, &selected, NULL);
    if (choice < 0 || choice == (int)next.mode) return;
    display_mode_t previous = next.mode;
    next.mode = (display_mode_t)choice;
    esp_err_t error = save_value("display_mode", next.mode, previous);
    if (error) menu_notice("SETTINGS ERROR", "COULD NOT SAVE DISPLAY MODE", "PREVIOUS VALUE KEPT");
    else { display_config_set(&next); ESP_LOGI(TAG, "PLAYER_SETTINGS,mode,%u", next.mode); }
}

static void change_border(void)
{
    const char *items[BORDER_COUNT];
    for (unsigned i = 0; i < BORDER_COUNT; ++i) items[i] = display_config_border_name((game_border_t)i);
    display_config_t next = display_config_get(); unsigned selected = next.border;
    int choice = menu_choose("GAME BORDER", "", items, BORDER_COUNT, &selected, NULL);
    if (choice < 0 || choice == (int)next.border) return;
    game_border_t previous = next.border;
    next.border = (game_border_t)choice;
    esp_err_t error = save_value("border", next.border, previous);
    if (error) menu_notice("SETTINGS ERROR", "COULD NOT SAVE BORDER", "PREVIOUS VALUE KEPT");
    else { display_config_set(&next); ESP_LOGI(TAG, "PLAYER_BORDER,border,%u", next.border); }
}

static void change_rotation(void)
{
    const char *items[SCREEN_ROTATE_COUNT] = {"0 DEGREES", "90 DEGREES", "180 DEGREES", "270 DEGREES"};
    display_config_t next = display_config_get(); unsigned selected = next.rotation;
    int choice = menu_choose("SCREEN ROTATION", "", items, SCREEN_ROTATE_COUNT, &selected, NULL);
    if (choice < 0 || choice == (int)next.rotation) return;
    screen_rotation_t previous = next.rotation;
    next.rotation = (screen_rotation_t)choice;
    esp_err_t error = save_value("rotation", next.rotation, previous);
    if (error) menu_notice("SETTINGS ERROR", "COULD NOT SAVE ROTATION", "PREVIOUS VALUE KEPT");
    else { display_config_set(&next); ESP_LOGI(TAG, "PLAYER_ROTATION,rotation,%u", next.rotation); }
}

static void carousel_settings(void)
{
    unsigned selected = 0;
    while (!screen_failed()) {
        display_config_t config = display_config_get();
        char toggle[32], every[32];
        const unsigned index = carousel_index(config.carousel_minutes);
        snprintf(toggle, sizeof toggle, "RANDOM GAMES: %s", config.carousel ? "ON" : "OFF");
        snprintf(every, sizeof every, "SWITCH EVERY: %u MIN", (unsigned)carousel_steps[index]);
        const char *items[] = {toggle, every}; int direction = 1;
        int choice = menu_choose("CAROUSEL", "", items, 2, &selected, &direction);
        if (choice < 0) return;
        if (choice == 0) {
            if (save_value("carousel", !config.carousel, config.carousel))
                menu_notice("SETTINGS ERROR", "COULD NOT SAVE SETTING", "PREVIOUS VALUE KEPT");
            else { config.carousel = !config.carousel; display_config_set(&config); }
            continue;
        }
        /* Left lowers, right or A raises; clamped rather than wrapping from an hour to a minute. */
        unsigned next = index;
        if (direction < 0 && next > 0) --next;
        else if (direction > 0 && next + 1 < CAROUSEL_STEPS) ++next;
        if (next == index && carousel_minutes_valid(config.carousel_minutes)) continue;
        if (save_value("carousel_min", carousel_steps[next], config.carousel_minutes))
            menu_notice("SETTINGS ERROR", "COULD NOT SAVE TIME", "PREVIOUS VALUE KEPT");
        else { config.carousel_minutes = carousel_steps[next]; display_config_set(&config); }
    }
}

static void dither_settings(void)
{
    unsigned selected = 0;
    while (!screen_failed()) {
        const display_config_t config = display_config_get(); char black[32], white[32];
        snprintf(black, sizeof black, "BLACK PULSES: %u", config.dither_black_pulses);
        snprintf(white, sizeof white, "WHITE PULSES: %u", config.dither_white_pulses);
        const char *items[] = {black, white}; int direction = 1;
        int choice = menu_choose("DITHERED TUNING", "", items, 2, &selected, &direction);
        if (choice < 0) return;
        display_preset_t next = *display_preset_active(DISPLAY_DITHERED);
        uint8_t *value = choice == 0 ? &next.black_pulses : &next.white_pulses;
        *value = (uint8_t)(1 + ((*value - 1 + (direction > 0 ? 1 : DITHER_PULSES_MAX - 1)) % DITHER_PULSES_MAX));
        if (preset_ui_activate(&next, true)) menu_notice("SETTINGS ERROR", "COULD NOT SAVE PULSES", "PREVIOUS VALUE KEPT");
        else ESP_LOGI(TAG, "PLAYER_DITHER_SETTINGS,black,%u,white,%u", next.black_pulses, next.white_pulses);
    }
}

static void display_settings(void)
{
    unsigned selected = 0;
    while (!screen_failed()) {
        const display_config_t config = display_config_get();
        char mode[40], border[40], preset[40], rotation[40], active[PRESET_NAME_MAX + 3];
        snprintf(mode, sizeof mode, "MODE: %s", display_config_mode_name());
        snprintf(border, sizeof border, "BORDER: %s", display_config_border_name(config.border));
        display_preset_active_label(config.mode, active, sizeof active);
        snprintf(preset, sizeof preset, "PRESET: %s", active);
        snprintf(rotation, sizeof rotation, "ROTATION: %u", (unsigned)config.rotation * 90u);
        /* Rotation is the last row, so the rows above keep their positions in both modes. */
        const char *items[6] = {mode, border, preset, "TUNING"};
        unsigned count = 4;
        if (config.mode == DISPLAY_GRAYSCALE) items[count++] = "TRANSITION TEST";
        items[count++] = rotation;
        int choice = menu_choose("DISPLAY", "", items, count, &selected, NULL);
        if (choice < 0) return;
        if (choice == (int)count - 1) change_rotation();
        else if (choice == 0) change_mode();
        else if (choice == 1) change_border();
        else if (choice == 2) preset_ui_open();
        else if (choice == 3) { if (config.mode == DISPLAY_DITHERED) dither_settings(); else gray_settings_ui_open(); }
        else gray_settings_ui_transition_test();
    }
}

/* Scans for Bluetooth LE gamepads and pairs the one picked from the list. The board buttons drive it:
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
{
    unsigned selected = 0;
    while (!screen_failed()) {
        char connection[40];
        snprintf(connection, sizeof connection, "CONNECTION: %s", controller_transport() == CONTROLLER_USB ? "USB" : "BLUETOOTH");
        /* Pairing rows exist only while Bluetooth is selected; Forget only once a gamepad is paired. */
        const bool bluetooth = controller_transport() == CONTROLLER_BLUETOOTH;
        const bool can_forget = bluetooth && controller_pad_paired();
        const char *items[] = {connection, "MAP BUTTONS", "PAIR GAMEPAD", "FORGET GAMEPAD"};
        int choice = menu_choose("CONTROLLER", "", items, !bluetooth ? 2 : can_forget ? 4 : 3, &selected, NULL);
        if (choice < 0) return;
        if (choice == 1) { input_remap(); continue; }
        if (choice == 2) { pair_gamepad(); continue; }
        if (choice == 3) {
            if (menu_confirm("FORGET GAMEPAD", "UNPAIR IT, THEN PAIR ANOTHER", "FORGET")) controller_forget_pad();
            continue;
        }
        const char *connections[] = {"BLUETOOTH", "USB"};
        unsigned transport = controller_transport();
        choice = menu_choose("CONNECTION", "", connections, 2, &transport, NULL);
        if (choice < 0 || (choice == (int)controller_transport() && controller_running())) continue;
        if (choice == CONTROLLER_USB && !menu_confirm("USB CONTROLLER", "HOLD BOTH BUTTONS FOR SERIAL", "USE USB")) continue;
        if (!screen_close()) return;
        esp_err_t error = controller_select((controller_transport_t)choice);
        if (error) menu_notice("CONTROLLER ERROR", "COULD NOT SWITCH CONTROLLER", controller_running() ? "PREVIOUS CONTROLLER KEPT" : "CHOOSE ONE AGAIN");
    }
}

static void sound_settings(void)
{
    unsigned selected = 0;
    while (!screen_failed()) {
        char sound[32], level[32];
        snprintf(sound, sizeof sound, "GAME SOUND: %s", sound_enabled ? "ON" : "OFF");
        snprintf(level, sizeof level, "VOLUME: %u", volume);
        const char *items[] = {sound, level}; int direction = 1;
        int choice = menu_choose("SOUND", "", items, 2, &selected, &direction);
        if (choice < 0) return;
        if (choice == 0) {
            if (save_value("sound", !sound_enabled, sound_enabled))
                menu_notice("SETTINGS ERROR", "COULD NOT SAVE SETTING", "PREVIOUS VALUE KEPT");
            else { sound_enabled = !sound_enabled; game_audio_set_enabled(sound_enabled); }
            continue;
        }
        /* Left lowers; right or A raises. Clamped rather than wrapping from loud to quiet. */
        unsigned next = direction < 0 ? (volume > 1 ? volume - 1u : 1u) : (volume < VOLUME_MAX ? volume + 1u : VOLUME_MAX);
        if (next == volume) continue;
        if (save_value("volume", (uint8_t)next, volume))
            menu_notice("SETTINGS ERROR", "COULD NOT SAVE VOLUME", "PREVIOUS VALUE KEPT");
        else {
            volume = (uint8_t)next;
            game_audio_set_volume(volume_db(volume));
            ESP_LOGI(TAG, "PLAYER_VOLUME,level,%u", volume);
        }
    }
}

static void fps_settings(void)
{
    unsigned selected = 0;
    while (!screen_failed()) {
        display_config_t config = display_config_get(); char label[32];
        snprintf(label, sizeof label, "SHOW FPS: %s", config.show_fps ? "ON" : "OFF");
        const char *items[] = {label};
        if (menu_choose("FPS", "", items, 1, &selected, NULL) < 0) return;
        if (save_value("show_fps", !config.show_fps, config.show_fps))
            menu_notice("SETTINGS ERROR", "COULD NOT SAVE SETTING", "PREVIOUS VALUE KEPT");
        else { config.show_fps = !config.show_fps; display_config_set(&config); }
    }
}

void settings_ui_open(void)
{
    unsigned selected = 0;
    const char *items[] = {"DISPLAY", "CONTROLLER", "SOUND", "FPS", "CAROUSEL"};
    while (!screen_failed()) {
        int choice = menu_choose("SETTINGS", "", items, 5, &selected, NULL);
        if (choice < 0) return;
        if (choice == 0) display_settings();
        else if (choice == 1) controller_settings();
        else if (choice == 2) sound_settings();
        else if (choice == 3) fps_settings();
        else carousel_settings();
    }
}
