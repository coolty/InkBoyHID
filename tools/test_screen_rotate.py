#!/usr/bin/env python3
"""Screen rotation: panel frames and packed game images against a per-pixel reference."""
from host_build import run

CODE = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "screen_rotate.h"

static uint32_t seed = 1;
static uint8_t random_byte(void) { seed = seed * 1664525u + 1013904223u; return (uint8_t)(seed >> 24); }
static unsigned panel_get(const uint8_t *f, unsigned x, unsigned y) { return (f[y * 25 + x / 8] >> (7 - x % 8)) & 1u; }
static void panel_set(uint8_t *f, unsigned x, unsigned y, unsigned v)
{ if (v) f[y * 25 + x / 8] |= (uint8_t)(0x80u >> (x % 8)); else f[y * 25 + x / 8] &= (uint8_t)~(0x80u >> (x % 8)); }
static unsigned packed_get(const uint8_t *p, unsigned row_pixels, unsigned x, unsigned y)
{ return (p[y * (row_pixels / 4) + x / 4] >> ((x % 4) * 2)) & 3u; }

static void panel_frames(void)
{
    static uint8_t src[5000], got[5000], want[5000], back[5000];
    for (screen_rotation_t r = 0; r < SCREEN_ROTATE_COUNT; ++r) {
        for (unsigned round = 0; round < 20; ++round) {
            for (unsigned i = 0; i < 5000; ++i) src[i] = random_byte();
            memset(want, 0, sizeof(want));
            for (unsigned y = 0; y < 200; ++y) for (unsigned x = 0; x < 200; ++x) {
                unsigned dx, dy;
                screen_rotate_point(r, x, y, &dx, &dy);
                panel_set(want, dx, dy, panel_get(src, x, y));
            }
            screen_rotate_panel(r, src, got);
            if (memcmp(got, want, sizeof(got))) { fprintf(stderr, "panel rotation %u differs\n", r); assert(0); }
        }
    }
    /* Four quarter turns, and two half turns, return the original. */
    for (unsigned i = 0; i < 5000; ++i) src[i] = random_byte();
    memcpy(back, src, sizeof(back));
    for (unsigned n = 0; n < 4; ++n) { screen_rotate_panel(SCREEN_ROTATE_90, back, got); memcpy(back, got, sizeof(back)); }
    assert(!memcmp(back, src, sizeof(back)));
    screen_rotate_panel(SCREEN_ROTATE_180, src, got); screen_rotate_panel(SCREEN_ROTATE_180, got, back);
    assert(!memcmp(back, src, sizeof(back)));
    screen_rotate_panel(SCREEN_ROTATE_90, src, got); screen_rotate_panel(SCREEN_ROTATE_270, got, back);
    assert(!memcmp(back, src, sizeof(back)));
}

static void packed_images(void)
{
    static uint8_t src[SCREEN_ROTATE_GAME_BYTES], got[SCREEN_ROTATE_GAME_BYTES], want[SCREEN_ROTATE_GAME_BYTES];
    static const unsigned sizes[][2] = {{160, 144}, {200, 180}, {8, 4}, {4, 8}, {40, 24}};
    for (screen_rotation_t r = 0; r < SCREEN_ROTATE_COUNT; ++r)
        for (unsigned s = 0; s < 5; ++s) {
            const unsigned w = sizes[s][0], h = sizes[s][1], dw = r % 2 ? h : w, dh = r % 2 ? w : h;
            for (unsigned i = 0; i < w * h / 4; ++i) src[i] = random_byte();
            memset(want, 0, sizeof(want)); memset(got, 0, sizeof(got));
            for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
                unsigned dx = x, dy = y;
                if (r == SCREEN_ROTATE_90) { dx = h - 1 - y; dy = x; }
                else if (r == SCREEN_ROTATE_180) { dx = w - 1 - x; dy = h - 1 - y; }
                else if (r == SCREEN_ROTATE_270) { dx = y; dy = w - 1 - x; }
                want[dy * (dw / 4) + dx / 4] |= (uint8_t)(packed_get(src, w, x, y) << ((dx % 4) * 2));
            }
            screen_rotate_packed(r, src, w, h, got, dw, 0);
            assert(!memcmp(got, want, dw * dh / 4));
        }
}

/* The game image and the panel must turn the same way: put a game into an upright canvas, rotate the
 * canvas pixel by pixel, and compare with the rotated game placed at the rotated view. */
static void game_matches_panel(void)
{
    static uint8_t game[SCREEN_ROTATE_GAME_BYTES], turned[SCREEN_ROTATE_GAME_BYTES];
    static uint8_t canvas[200][200], rotated[200][200];
    const game_view_t views[] = {GAME_VIEW_CLASSIC, GAME_VIEW_FILL};
    for (screen_rotation_t r = 0; r < SCREEN_ROTATE_COUNT; ++r)
        for (unsigned v = 0; v < 2; ++v) {
            const game_view_t view = views[v];
            for (unsigned i = 0; i < (unsigned)view.width * view.height / 4; ++i) game[i] = random_byte();
            memset(canvas, 0, sizeof(canvas)); memset(rotated, 0, sizeof(rotated));
            for (unsigned y = 0; y < view.height; ++y) for (unsigned x = 0; x < view.width; ++x)
                canvas[view.y + y][view.x + x] = (uint8_t)packed_get(game, view.width, x, y);
            for (unsigned y = 0; y < 200; ++y) for (unsigned x = 0; x < 200; ++x) {
                unsigned dx, dy;
                screen_rotate_point(r, x, y, &dx, &dy);
                rotated[dy][dx] = canvas[y][x];
            }
            const game_view_t out = screen_rotate_game(r, game, view, turned);
            assert(out.x % 4 == 0 && out.width % 4 == 0 && out.x + out.width <= 200 && out.y + out.height <= 200);
            assert((unsigned)out.width * out.height / 4 <= SCREEN_ROTATE_GAME_BYTES);
            /* Inside the widened view every pixel agrees; the widened columns are white (zero). */
            for (unsigned y = 0; y < out.height; ++y) for (unsigned x = 0; x < out.width; ++x)
                assert(packed_get(turned, out.width, x, y) == rotated[out.y + y][out.x + x]);
            /* Nothing of the image lies outside it. */
            for (unsigned y = 0; y < 200; ++y) for (unsigned x = 0; x < 200; ++x)
                if (rotated[y][x] && !(x >= out.x && x < (unsigned)out.x + out.width && y >= out.y && y < (unsigned)out.y + out.height)) assert(0);
        }
    /* Both layouts are centred, so a half turn keeps the view where it was; a quarter turn keeps it aligned. */
    assert(screen_rotate_game(SCREEN_ROTATE_180, (uint8_t[SCREEN_ROTATE_GAME_BYTES]){0}, GAME_VIEW_CLASSIC, turned).x == 20);
    game_view_t q = screen_rotate_game(SCREEN_ROTATE_90, game, GAME_VIEW_CLASSIC, turned);
    assert(q.x == 28 && q.y == 20 && q.width == 144 && q.height == 160);
    q = screen_rotate_game(SCREEN_ROTATE_270, game, GAME_VIEW_FILL, turned);
    assert(q.x == 8 && q.y == 0 && q.width == 184 && q.height == 200);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "panel")) panel_frames();
    else if (!strcmp(argv[1], "packed")) packed_images();
    else if (!strcmp(argv[1], "game")) game_matches_panel();
    else assert(0);
    return 0;
}
'''

run(CODE, scenarios=[[x] for x in ('panel', 'packed', 'game')])
print('PASS: panel frames and packed game images rotate like a per-pixel reference, and the game image and view turn with the panel')
