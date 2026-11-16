#!/usr/bin/env python3
"""Rotation through the real fast display backend: dithered frames land turned, the grayscale planner targets follow."""
from host_build import run

CODE = r'''
#include "fast_display.c"
#include <assert.h>
#include <stdlib.h>
#include "host_fakes.h"
#include "host_panel.h"

static uint32_t seed = 9;
static uint8_t random_byte(void) { seed = seed * 1664525u + 1013904223u; return (uint8_t)(seed >> 24); }
static uint8_t binary[5000], second[5000], expect[5000], game[5760];
#define H ((ssd1681_handle_t)1)

static void turned(screen_rotation_t r, const uint8_t *src, uint8_t *dst)
{
    memset(dst, 0, 5000);
    for (unsigned y = 0; y < 200; ++y) for (unsigned x = 0; x < 200; ++x) {
        unsigned dx, dy;
        screen_rotate_point(r, x, y, &dx, &dy);
        if (src[y * 25 + x / 8] & (0x80u >> (x % 8))) dst[dy * 25 + dx / 8] |= (uint8_t)(0x80u >> (dx % 8));
    }
}

static void dithered(screen_rotation_t r)
{
    host_fakes_reset(); host_panel_reset();
    fast_display_set_rotation(r);
    for (unsigned i = 0; i < 5000; ++i) binary[i] = random_byte();
    assert(!fast_display_full(H, binary, DISPLAY_DITHERED));
    turned(r, binary, expect);
    assert(!memcmp(host_panel.physical, expect, 5000)); /* The clean reference is turned. */
    memcpy(second, binary, sizeof(second));
    for (unsigned n = 0; n < 400; ++n) second[random_byte() * 19u % 5000u] ^= random_byte();
    assert(!fast_display_update(H, second));
    while (fast_display_pending(H)) assert(!fast_display_update(H, second));
    turned(r, second, expect);
    assert(!memcmp(host_panel.physical, expect, 5000)); /* Every drive scan was in panel coordinates. */
    assert(!fast_display_finish(H, second) && !memcmp(host_panel.physical, expect, 5000));
}

static void grayscale(screen_rotation_t r)
{
    host_fakes_reset(); host_panel_reset();
    fast_display_set_rotation(r);
    for (unsigned i = 0; i < 5000; ++i) binary[i] = random_byte();
    for (unsigned i = 0; i < sizeof(game); ++i) game[i] = random_byte();
    fast_display_set_gray_game(game);
    assert(!fast_display_full(H, binary, DISPLAY_GRAYSCALE));
    for (unsigned y = 0; y < 200; ++y) for (unsigned x = 0; x < 200; ++x) {
        const bool inside = x >= 20 && x < 180 && y >= 28 && y < 172;
        unsigned want = (binary[y * 25 + x / 8] & (0x80u >> (x % 8))) ? 3 : 0;
        if (inside) {
            const unsigned gx = x - 20, gy = y - 28;
            want = 3 - ((game[gy * 40 + gx / 4] >> ((gx % 4) * 2)) & 3);
        }
        unsigned dx, dy;
        screen_rotate_point(r, x, y, &dx, &dy);
        if (gray_plan->pixel[dy * 200 + dx].target != want) { fprintf(stderr, "r=%u pixel %u,%u\n", r, x, y); assert(0); }
    }
    /* Drawing continues in the same orientation until the image settles. */
    for (unsigned n = 0; n < 40; ++n) assert(!fast_display_update(H, binary));
    assert(!fast_display_full(H, binary, DISPLAY_DITHERED));
}

static void no_memory(void)
{
    host_fakes_reset(); host_panel_reset();
    fast_display_set_rotation(SCREEN_ROTATE_90);
    memset(binary, 255, sizeof(binary));
    host_heap_fail_all = true;
    assert(fast_display_full(H, binary, DISPLAY_DITHERED) == ESP_ERR_NO_MEM);
    host_heap_fail_all = false;
    fast_display_set_rotation(SCREEN_ROTATE_0);
    assert(!fast_display_full(H, binary, DISPLAY_DITHERED) && !rotated_frame && !rotated_game);
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    const screen_rotation_t r = (screen_rotation_t)atoi(argv[2]);
    if (!strcmp(argv[1], "dithered")) dithered(r);
    else if (!strcmp(argv[1], "gray")) grayscale(r);
    else if (!strcmp(argv[1], "nomem")) no_memory();
    else assert(0);
    return 0;
}
'''

run(CODE, modules=['gray_plan.c', 'gray_plan_parallel.c', 'drive_counters.c'], fakes=['host_panel.c'],
    scenarios=[[m, str(r)] for m in ('dithered', 'gray') for r in range(4)] + [['nomem', '0']])
print('PASS: all four rotations: dithered clean/drive/final frames in panel coordinates, grayscale targets follow, allocation failure')
