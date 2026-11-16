#!/usr/bin/env python3
"""Random carousel in the real game session: which game is picked, the Games list entry and arming."""
from host_build import run, DISPLAY_MODULES, SESSION_FAKES, GB_VIDEO

CODE = r'''
#include "game_session.c"
#include <assert.h>
#include "fake_game_storage.h"
#include "fake_gb_emulator.h"
#include "fake_menu.h"
#include "host_fakes.h"
#include "host_panel.h"

static const char *const NAMES[] = {"a.gb", "b.gb", "c.gb"};

static game_entry_t entry(const char *name)
{
    game_entry_t e;
    memset(&e, 0, sizeof(e));
    strcpy(e.name, name);
    return e;
}

static void start(unsigned count, const char *current, bool enabled, unsigned minutes)
{
    host_fakes_reset();
    host_panel_reset();
    fake_gb_reset();
    fake_storage_reset();
    strcpy(host_task_autorun_name, "game_save");
    assert(game_session_start());
    fake_storage.rom_size = 32768;
    for (unsigned i = 0; i < count; ++i) fake_storage.listed[i] = entry(NAMES[i]);
    fake_storage.list_count = count;
    if (current) { game_entry_t first = entry(current); assert(load_game(&first) && game_ready); }
    display_config_t config = display_config_get();
    config.carousel = enabled;
    config.carousel_minutes = (uint8_t)minutes;
    display_config_set(&config);
    fake_menu_notices = 0;
}

static void picks_another_game(void)
{
    /* Candidates are a and c, so even picks give a and odd picks give c, however large the number. */
    for (uint32_t value = 0; value < 8; ++value) {
        start(3, "b.gb", true, 5);
        host_random_value = value;
        assert(game_session_start_random() && game_session_carousel_active());
        assert(!strcmp(current_game.name, value % 2 ? "c.gb" : "a.gb") && game_ready);
        assert(carousel_us == 5 * 60000000LL);
    }
    start(3, "a.gb", true, 5);
    host_random_value = 0xfffffffeu;
    assert(game_session_start_random() && strcmp(current_game.name, "a.gb"));
    start(3, "c.gb", true, 5);
    host_random_value = 1;
    assert(game_session_start_random() && !strcmp(current_game.name, "b.gb"));
}

static void single_game_and_defaults(void)
{
    start(1, "a.gb", true, 0);
    assert(game_session_start_random() && !strcmp(current_game.name, "a.gb"));
    assert(carousel_us == 10 * 60000000LL); /* An unset time means ten minutes. */
    game_session_stop_carousel();
    assert(!game_session_carousel_active());
    start(3, NULL, true, 5); /* No game played yet: any of the three. */
    host_random_value = 2;
    assert(game_session_start_random() && !strcmp(current_game.name, "c.gb"));
}

static void failures_leave_it_off(void)
{
    start(0, NULL, true, 5);
    assert(!game_session_start_random() && !game_session_carousel_active() && fake_menu_notices == 1);
    start(3, "b.gb", true, 5);
    fake_storage.list_error = ESP_FAIL;
    assert(!game_session_start_random() && !game_session_carousel_active() && fake_menu_notices == 1);
    fake_storage.list_error = ESP_OK;
    fake_storage.read_error = ESP_ERR_NOT_SUPPORTED;
    assert(!game_session_start_random() && !game_session_carousel_active());
}

static const char *first_rows[3];
static int menu_pick;
static int scripted(const char *title, const char *subtitle, const char *const *items, unsigned count,
                    unsigned *selected, int *direction)
{
    (void)subtitle; (void)direction;
    if (strcmp(title, "GAMES")) return -1;
    for (unsigned i = 0; i < 3 && i < count; ++i) first_rows[i] = items[i];
    assert(count == (display_config_get().carousel ? 5u : 4u) && *selected >= 1);
    return menu_pick;
}

static void games_list(void)
{
    fake_menu_hook = scripted;
    /* Enabled: the random entry is the first visible row, and the games follow it. */
    start(3, NULL, true, 5);
    fake_menu_hook = scripted;
    menu_pick = 1;
    host_random_value = 1;
    assert(game_session_select() && game_session_carousel_active() && !strcmp(first_rows[1], "START RANDOM GAME"));
    assert(!strcmp(current_game.name, "b.gb")); /* One candidate shift: a, b, c with none excluded gives index 1. */
    start(3, NULL, true, 5);
    fake_menu_hook = scripted;
    menu_pick = 4; /* The third game. */
    assert(game_session_select() && !game_session_carousel_active() && !strcmp(current_game.name, "c.gb"));
    assert(!strcmp(first_rows[2], "a.gb"));
    /* Disabled: no extra row, and the first game is row one as before. */
    start(3, NULL, false, 5);
    fake_menu_hook = scripted;
    menu_pick = 1;
    assert(game_session_select() && !game_session_carousel_active() && !strcmp(current_game.name, "a.gb"));
    assert(!strcmp(first_rows[1], "a.gb"));
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "picks")) picks_another_game();
    else if (!strcmp(argv[1], "single")) single_game_and_defaults();
    else if (!strcmp(argv[1], "failures")) failures_leave_it_off();
    else if (!strcmp(argv[1], "list")) games_list();
    else assert(0);
    return 0;
}
'''

run(CODE, modules=DISPLAY_MODULES, fakes=SESSION_FAKES, extra_sources=[GB_VIDEO],
    scenarios=[[x] for x in ('picks', 'single', 'failures', 'list')])
print('PASS: carousel picks another game, never repeats the current one, defaults to ten minutes, fails safely; the Games list shows the random entry first only when enabled')
