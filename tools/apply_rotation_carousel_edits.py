#!/usr/bin/env python3
"""Screen rotation (Display settings) and the random carousel (Settings, Games list).

Run from the repository root after the earlier edit scripts. All or nothing: every edit anchors on exact
existing text, and if any is missing or already applied nothing is written and the script names it."""
import sys
from pathlib import Path

EDITS = {}


def edit(path, old, new):
    EDITS.setdefault(path, []).append((old, new))


# --- Shared configuration ---------------------------------------------------------------------
edit('main/display_config.h', '#include "display_mode.h"', '#include "display_mode.h"\n#include "screen_rotate.h"')
edit('main/display_config.h',
     '    game_border_t border;\n} display_config_t;',
     '''    game_border_t border;
    screen_rotation_t rotation;           /* Turns the whole screen, menus and game. */
    bool carousel;                        /* Games list offers "Start random game". */
    uint8_t carousel_minutes;             /* Play time before the next random game; 0 means ten. */
} display_config_t;''')
edit('main/display_config.c',
     '    .border = BORDER_BLACK_LOGO,\n};',
     '    .border = BORDER_BLACK_LOGO,\n    .carousel_minutes = 10,\n};')
# (main/game_view.h is copied by the installer: its maximum grows for rotated full-screen games.)

# --- Display backend: frames and the game image are turned on their way in --------------------
edit('main/fast_display.h', '#include "ssd1681_fast.h"', '#include "ssd1681_fast.h"\n#include "screen_rotate.h"')
edit('main/fast_display.h',
     'esp_err_t fast_display_set_dither_pulses(unsigned black, unsigned white);',
     '''esp_err_t fast_display_set_dither_pulses(unsigned black, unsigned white);
/* Turns everything drawn upright (menus, HUD, game) for a panel mounted otherwise. Takes effect at the
 * next fast_display_full, which every orientation change must be followed by. */
void fast_display_set_rotation(screen_rotation_t rotation);''')

edit('main/fast_display.c',
     'static DMA_ATTR uint8_t gray_follow_black[SSD1681_FRAME_BYTES], gray_follow_white[SSD1681_FRAME_BYTES];',
'''/* Screen rotation. Everything above this file draws upright; the panel, the dithered counters and the
 * grayscale planner work in panel coordinates, so frames and the game image are turned here, once, on the
 * way in. The setting is taken at fast_display_full. The extra buffers exist only while rotated. */
static screen_rotation_t rotation, active_rotation;
static uint8_t *rotated_frame, *rotated_game;

void fast_display_set_rotation(screen_rotation_t value)
{
    rotation = value < SCREEN_ROTATE_COUNT ? value : SCREEN_ROTATE_0;
}

static esp_err_t rotation_resources(void)
{
    active_rotation = rotation;
    if (active_rotation == SCREEN_ROTATE_0) {
        heap_caps_free(rotated_frame); heap_caps_free(rotated_game);
        rotated_frame = rotated_game = NULL;
        return ESP_OK;
    }
    if (!rotated_frame) rotated_frame = heap_caps_malloc(SSD1681_FRAME_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!rotated_game) rotated_game = heap_caps_malloc(SCREEN_ROTATE_GAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rotated_frame || !rotated_game) { active_rotation = SCREEN_ROTATE_0; return ESP_ERR_NO_MEM; }
    return ESP_OK;
}

static const uint8_t *oriented(const uint8_t *frame)
{
    if (active_rotation == SCREEN_ROTATE_0) return frame;
    screen_rotate_panel(active_rotation, frame, rotated_frame);
    return rotated_frame;
}

/* The game image for the planner, turned with its view when rotated. */
static const uint8_t *gray_oriented_game(void)
{
    if (!gray_game || !gray_plan || active_rotation == SCREEN_ROTATE_0) return gray_game;
    gray_plan->view = screen_rotate_game(active_rotation, gray_game, game_view_or_classic(gray_view), rotated_game);
    return rotated_game;
}

static DMA_ATTR uint8_t gray_follow_black[SSD1681_FRAME_BYTES], gray_follow_white[SSD1681_FRAME_BYTES];''')
edit('main/fast_display.c',
     '    owner = NULL; prepared = false;\n    const bool gray = mode == DISPLAY_GRAYSCALE;',
     '''    owner = NULL; prepared = false;
    esp_err_t turned = rotation_resources();
    if (turned) return turned;
    frame = oriented(frame);
    const bool gray = mode == DISPLAY_GRAYSCALE;''')
edit('main/fast_display.c',
     'gray_plan_parallel_target(gray_plan, frame, gray_game);',
     'gray_plan_parallel_target(gray_plan, frame, gray_oriented_game());')
edit('main/fast_display.c',
     '    ssd1681_timing_t timing = {0};\n    esp_err_t error = ssd1681_vendor_full(d, frame, SSD1681_FRAME_BYTES, &timing);',
     '    ssd1681_timing_t timing = {0};\n    esp_err_t error = ssd1681_vendor_full(d, oriented(frame), SSD1681_FRAME_BYTES, &timing);')
edit('main/fast_display.c',
     '    if (selected == DISPLAY_GRAYSCALE) return gray_update(d, frame);',
     '    frame = oriented(frame);\n    if (selected == DISPLAY_GRAYSCALE) return gray_update(d, frame);')
edit('main/fast_display.c', '    unsigned holds=gray_holds;', '    unsigned holds=gray_holds;\n    const uint8_t *game = gray_oriented_game();')
edit('main/fast_display.c',
     'gray_plan_parallel_prepare_pair(gray_plan, frame, gray_game, black, white,',
     'gray_plan_parallel_prepare_pair(gray_plan, frame, game, black, white,')
edit('main/fast_display.c',
     'gray_plan_parallel_prepare(gray_plan, frame, gray_game, black, white);',
     'gray_plan_parallel_prepare(gray_plan, frame, game, black, white);')

# --- Menus and gameplay follow the setting through the screen module ---------------------------
edit('main/screen.c', 'static uint8_t menu_dither_black, menu_dither_white;',
     'static uint8_t menu_dither_black, menu_dither_white;\nstatic screen_rotation_t menu_rotation;')
edit('main/screen.c', '    return fast_display_full(d, panel, config.mode);',
     '    fast_display_set_rotation(config.rotation);\n    return fast_display_full(d, panel, config.mode);')
edit('main/screen.c',
     '''    menu_dither_black = config.dither_black_pulses; menu_dither_white = config.dither_white_pulses;
    /* Gameplay is continuous; start the menu's bounded refresh counter anew. */''',
     '''    menu_dither_black = config.dither_black_pulses; menu_dither_white = config.dither_white_pulses;
    menu_rotation = config.rotation;
    /* Gameplay is continuous; start the menu's bounded refresh counter anew. */''')
edit('main/screen.c', '    bool mode_changed = menu_mode != config.mode ||',
     '    bool mode_changed = menu_mode != config.mode || menu_rotation != config.rotation ||')
edit('main/screen.c',
     '''        menu_analog = false; menu_settling = 0; menu_mode = config.mode;
        menu_dither_black = config.dither_black_pulses; menu_dither_white = config.dither_white_pulses;
    } else if (!error) {''',
     '''        menu_analog = false; menu_settling = 0; menu_mode = config.mode;
        menu_dither_black = config.dither_black_pulses; menu_dither_white = config.dither_white_pulses;
        menu_rotation = config.rotation;
    } else if (!error) {''')

# --- Settings: Display > Rotation (last row) and the Carousel category -------------------------
edit('main/settings_ui.c',
     'static float volume_db(unsigned level) { return 3.0f * (float)level - 30.0f; }',
     '''static float volume_db(unsigned level) { return 3.0f * (float)level - 30.0f; }
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
}''')
edit('main/settings_ui.c',
     '    ESP_LOGI(TAG, "PLAYER_BORDER,border,%u", config.border);',
     '''    ESP_LOGI(TAG, "PLAYER_BORDER,border,%u", config.border);
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
    ESP_LOGI(TAG, "PLAYER_CAROUSEL_SETTINGS,enabled,%u,minutes,%u", config.carousel, config.carousel_minutes);''')
edit('main/settings_ui.c', 'static void dither_settings(void)\n{',
'''static void change_rotation(void)
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
{''')
edit('main/settings_ui.c',
     'char mode[40], border[40], preset[40], active[PRESET_NAME_MAX + 3];',
     'char mode[40], border[40], preset[40], rotation[40], active[PRESET_NAME_MAX + 3];')
edit('main/settings_ui.c',
     '        snprintf(preset, sizeof preset, "PRESET: %s", active);',
     '        snprintf(preset, sizeof preset, "PRESET: %s", active);\n'
     '        snprintf(rotation, sizeof rotation, "ROTATION: %u", (unsigned)config.rotation * 90u);')
edit('main/settings_ui.c',
'''        const char *items[] = {mode, border, preset, "TUNING", "TRANSITION TEST"};
        int choice = menu_choose("DISPLAY", "", items, config.mode == DISPLAY_GRAYSCALE ? 5 : 4, &selected, NULL);
        if (choice < 0) return;
        if (choice == 0) change_mode();
        else if (choice == 1) change_border();
        else if (choice == 2) preset_ui_open();
        else if (choice == 3) { if (config.mode == DISPLAY_DITHERED) dither_settings(); else gray_settings_ui_open(); }
        else gray_settings_ui_transition_test();''',
'''        /* Rotation is the last row, so the rows above keep their positions in both modes. */
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
        else gray_settings_ui_transition_test();''')
edit('main/settings_ui.c',
'''    const char *items[] = {"DISPLAY", "CONTROLLER", "SOUND", "FPS"};
    while (!screen_failed()) {
        int choice = menu_choose("SETTINGS", "", items, 4, &selected, NULL);
        if (choice < 0) return;
        if (choice == 0) display_settings();
        else if (choice == 1) controller_settings();
        else if (choice == 2) sound_settings();
        else fps_settings();''',
'''    const char *items[] = {"DISPLAY", "CONTROLLER", "SOUND", "FPS", "CAROUSEL"};
    while (!screen_failed()) {
        int choice = menu_choose("SETTINGS", "", items, 5, &selected, NULL);
        if (choice < 0) return;
        if (choice == 0) display_settings();
        else if (choice == 1) controller_settings();
        else if (choice == 2) sound_settings();
        else if (choice == 3) fps_settings();
        else carousel_settings();''')

# --- Game session: the random pick, the timer, and the Games list entry -------------------------
edit('main/game_session.h', 'unsigned game_session_play(void);',
'''unsigned game_session_play(void);
/* Returned by game_session_play when the random carousel's time is up. */
#define GAME_SESSION_CAROUSEL 4u

/* Random carousel. start_random picks a game that is not the current one (unless it is the only one),
 * loads it, continues its saved checkpoint if it has one, and arms the timer from the Carousel settings.
 * game_session_play then returns GAME_SESSION_CAROUSEL after that much play. Stopping clears it. */
bool game_session_start_random(void);
void game_session_stop_carousel(void);
bool game_session_carousel_active(void);''')
edit('main/game_session.c', '#include "esp_memory_utils.h"', '#include "esp_memory_utils.h"\n#include "esp_random.h"')
edit('main/game_session.c', 'static uint32_t emulated_frames;',
     'static uint32_t emulated_frames;\nstatic bool carousel_on;\nstatic int64_t carousel_us;')
edit('main/game_session.c', '        if (!input_connected()) { reason = 3; break; }',
'''        if (!input_connected()) { reason = 3; break; }
        /* Random carousel: after the set time of play, hand over so the next game can start. */
        if (carousel_on && esp_timer_get_time() - started >= carousel_us) { reason = GAME_SESSION_CAROUSEL; break; }''')
edit('main/game_session.c', 'bool game_session_select(void)\n{',
'''bool game_session_carousel_active(void) { return carousel_on; }
void game_session_stop_carousel(void) { carousel_on = false; }

bool game_session_start_random(void)
{
    carousel_on = false;
    size_t count = 0; bool truncated = false;
    esp_err_t error = game_storage_list(games, GAME_CATALOG_MAX, &count, &truncated);
    if (error || !count) {
        menu_notice(error ? "CARD ERROR" : "NO GAMES", "PUT GB FILES IN CARD /ROMS", "POWER OFF BEFORE REMOVING CARD");
        return false;
    }
    /* Never the game just played, unless it is the only one. */
    size_t skip = count, candidates = count;
    if (count > 1 && game_ready)
        for (size_t i = 0; i < count; ++i)
            if (!strcmp(current_game.name, games[i].name)) { skip = i; --candidates; break; }
    size_t pick = (size_t)(esp_random() % candidates);
    if (pick >= skip) ++pick;
    if (!load_game(&games[pick])) return false;
    bool exists = false;
    if (game_storage_has_state(&current_game, current_rom_crc, current_rom_size, gb_emulator_state_size(), &exists) == ESP_OK && exists)
        (void)game_session_load_checkpoint();
    const unsigned configured = display_config_get().carousel_minutes, minutes = configured ? configured : 10u;
    carousel_us = (int64_t)minutes * 60000000LL;
    carousel_on = true;
    ESP_LOGI(TAG, "PLAYER_CAROUSEL,game,%s,minutes,%u", current_game.name, minutes);
    return true;
}

bool game_session_select(void)
{''')
edit('main/game_session.c',
'''    const char *items[GAME_CATALOG_MAX + 1];
    items[0] = MENU_BACK;
    for (size_t i = 0; i < count; ++i) items[i + 1] = games[i].name;
    unsigned selected = 1;
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(current_game.name, games[i].name)) selected = (unsigned)i + 1;''',
'''    /* With the random carousel enabled, "START RANDOM GAME" is the first visible row. */
    const bool random_entry = display_config_get().carousel;
    const unsigned first = random_entry ? 2u : 1u;
    const char *items[GAME_CATALOG_MAX + 2];
    items[0] = MENU_BACK;
    if (random_entry) items[1] = "START RANDOM GAME";
    for (size_t i = 0; i < count; ++i) items[i + first] = games[i].name;
    unsigned selected = first;
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(current_game.name, games[i].name)) selected = (unsigned)i + first;''')
edit('main/game_session.c', 'items, count + 1, &selected, NULL);', 'items, count + first, &selected, NULL);')
edit('main/game_session.c',
'''        if (!input_mapping_valid() && !input_bypassed() && !input_calibrate()) return false;
        if (load_game(&games[choice - 1])) {''',
'''        if (!input_mapping_valid() && !input_bypassed() && !input_calibrate()) return false;
        if (random_entry && choice == 1) return game_session_start_random();
        if (load_game(&games[choice - first])) {''')

edit('main/app.c',
'''            if (!screen_close()) break;
            input_wait_release(); input_clear_events();
            (void)game_session_play();
            if (screen_failed()) break;
            game_session_ensure_saved();
            selected = 0;''',
'''            /* One pass normally; the random carousel loops, saving each game and starting the next. */
            bool stopped = false;
            for (;;) {
                if (!screen_close()) { stopped = true; break; }
                input_wait_release(); input_clear_events();
                const unsigned reason = game_session_play();
                if (screen_failed()) { stopped = true; break; }
                game_session_ensure_saved();
                if (reason != GAME_SESSION_CAROUSEL || !game_session_start_random()) break;
            }
            if (stopped) break;
            selected = 0;''')
edit('main/app.c',
     '        input_set_bypass(false); /* Playing without a controller is chosen per game start. */',
     '        input_set_bypass(false); /* Playing without a controller is chosen per game start. */\n'
     '        game_session_stop_carousel(); /* The carousel only runs from its entry in the Games list. */')

# --- Tests and fakes ----------------------------------------------------------------------------
edit('tools/host_build.py', "'host_rtos.c', 'host_spi.c']", "'host_random.c', 'host_rtos.c', 'host_spi.c']")
edit('tools/host_fakes/host_fakes.h', 'void host_fakes_reset(void);',
     'void host_fakes_reset(void);\n\n/* esp_random() returns this value, so a test chooses the "random" game. */\nextern uint32_t host_random_value;')
edit('tools/test_menu.py',
     'esp_err_t fast_display_set_dither_pulses(unsigned black, unsigned white) { (void)black; (void)white; return ESP_OK; }',
     'esp_err_t fast_display_set_dither_pulses(unsigned black, unsigned white) { (void)black; (void)white; return ESP_OK; }\n'
     'void fast_display_set_rotation(screen_rotation_t rotation) { (void)rotation; }')
edit('tools/test_hardware_input.py',
     'esp_err_t fast_display_set_dither_pulses(unsigned b,unsigned w) { (void)b;(void)w;return ESP_OK; }',
     'esp_err_t fast_display_set_dither_pulses(unsigned b,unsigned w) { (void)b;(void)w;return ESP_OK; }\n'
     'void fast_display_set_rotation(screen_rotation_t r) { (void)r; }')
edit('tools/test_settings_ui.py',
     '        assert(count == 4 && !strcmp(items[0],"DISPLAY") && !strcmp(items[1],"CONTROLLER") && !strcmp(items[2],"SOUND") && !strcmp(items[3],"FPS"));',
     '        assert(count == 5 && !strcmp(items[0],"DISPLAY") && !strcmp(items[1],"CONTROLLER") && !strcmp(items[2],"SOUND") && !strcmp(items[3],"FPS") && !strcmp(items[4],"CAROUSEL"));')
edit('tools/test_settings_ui.py', 'static void usb_failure(void) {',
'''static void rotation_and_carousel(void) {
    start();
    push("SETTINGS",0); push("DISPLAY",4); push("SCREEN ROTATION",2); push("DISPLAY",-1);   /* Rotation is the last row. */
    push("SETTINGS",4); push("CAROUSEL",0); push("CAROUSEL",1); push("CAROUSEL",1); push("CAROUSEL",-1);
    push("SETTINGS",-1); finish();
    display_config_t c = display_config_get();
    assert(c.rotation == SCREEN_ROTATE_180 && host_nvs_u8("gamepad","rotation") == 2);
    /* The unset time shows as 10 and steps 15, then 20. */
    assert(c.carousel && host_nvs_u8("gamepad","carousel") == 1 && c.carousel_minutes == 20 && host_nvs_u8("gamepad","carousel_min") == 20);
    display_config_t reset = {DISPLAY_DITHERED,2,2,false,BORDER_BLACK_LOGO}; display_config_set(&reset);
    settings_ui_load_display(); settings_ui_load_toggles();
    assert(display_config_get().rotation == SCREEN_ROTATE_180 && display_config_get().carousel && display_config_get().carousel_minutes == 20);
    uint8_t bad = 77; host_nvs_seed("gamepad","rotation",HOST_NVS_U8,&bad,1); host_nvs_seed("gamepad","carousel_min",HOST_NVS_U8,&bad,1);
    display_config_set(&reset); settings_ui_load_toggles();
    assert(display_config_get().rotation == SCREEN_ROTATE_0 && display_config_get().carousel_minutes == 0); /* Unusable values are ignored. */
}
static void usb_failure(void) {''')
edit('tools/test_settings_ui.py',
     'else if(!strcmp(argv[1],"pair")) pair(2); else if(!strcmp(argv[1],"pair-none")) pair(0); else usb_failure();',
     'else if(!strcmp(argv[1],"pair")) pair(2); else if(!strcmp(argv[1],"pair-none")) pair(0);\n'
     '    else if(!strcmp(argv[1],"rotation")) rotation_and_carousel(); else usb_failure();')
edit('tools/test_settings_ui.py', "'pair','pair-none']", "'pair','pair-none','rotation']")




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
