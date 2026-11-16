#pragma once
#include <stdbool.h>

bool game_session_start(void);

bool game_session_ready(void);
const char *game_session_name(void);

bool game_session_select(void);
unsigned game_session_play(void);
/* Returned by game_session_play when the random carousel's time is up. */
#define GAME_SESSION_CAROUSEL 4u

/* Random carousel. start_random picks a game that is not the current one (unless it is the only one),
 * loads it, continues its saved checkpoint if it has one, and arms the timer from the Carousel settings.
 * game_session_play then returns GAME_SESSION_CAROUSEL after that much play. Stopping clears it. */
bool game_session_start_random(void);
void game_session_stop_carousel(void);
bool game_session_carousel_active(void);

bool game_session_ensure_saved(void);
void game_session_save_state(void);
bool game_session_load_checkpoint(void);
