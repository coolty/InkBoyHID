#pragma once
#include <stdint.h>
#include <string.h>
#include "game_view.h"

/* Quarter turns clockwise of the whole screen: 90 moves the top of the upright layout to the right edge.
 * Header only, so the host tests and every module can use it without extra link steps. */
typedef enum { SCREEN_ROTATE_0, SCREEN_ROTATE_90, SCREEN_ROTATE_180, SCREEN_ROTATE_270, SCREEN_ROTATE_COUNT } screen_rotation_t;

#define SCREEN_ROTATE_SIDE 200u
#define SCREEN_ROTATE_PANEL_BYTES 5000u
/* Largest rotated game image: the whole panel at two bits per pixel. */
#define SCREEN_ROTATE_GAME_BYTES (SCREEN_ROTATE_SIDE * SCREEN_ROTATE_SIDE / 4u)

/* Where the upright pixel (x, y) lands on the panel. */
static inline void screen_rotate_point(screen_rotation_t r, unsigned x, unsigned y, unsigned *dx, unsigned *dy)
{
    const unsigned last = SCREEN_ROTATE_SIDE - 1u;
    switch (r) {
    case SCREEN_ROTATE_90: *dx = last - y; *dy = x; break;
    case SCREEN_ROTATE_180: *dx = last - x; *dy = last - y; break;
    case SCREEN_ROTATE_270: *dx = y; *dy = last - x; break;
    default: *dx = x; *dy = y; break;
    }
}

static inline uint8_t screen_rotate_reverse8(uint8_t v)
{
    v = (uint8_t)((v >> 4) | (v << 4));
    v = (uint8_t)(((v & 0xccu) >> 2) | ((v & 0x33u) << 2));
    return (uint8_t)(((v & 0xaau) >> 1) | ((v & 0x55u) << 1));
}

/* Transposes an 8x8 bit block packed as eight bytes (byte k at bits 8k..8k+7). */
static inline uint64_t screen_rotate_flip_diagonal(uint64_t x)
{
    uint64_t t = (x ^ (x >> 7)) & 0x00aa00aa00aa00aaull;
    x ^= t ^ (t << 7);
    t = (x ^ (x >> 14)) & 0x0000cccc0000ccccull;
    x ^= t ^ (t << 14);
    t = (x ^ (x >> 28)) & 0x00000000f0f0f0f0ull;
    x ^= t ^ (t << 28);
    return x;
}

/* Rotates a 200x200 one-bit frame (25 bytes per row, bit 7 leftmost). src and dst must not overlap. */
static inline void screen_rotate_panel(screen_rotation_t r, const uint8_t *src, uint8_t *dst)
{
    enum { ROW = SCREEN_ROTATE_SIDE / 8, BLOCKS = ROW };
    if (r == SCREEN_ROTATE_180) {
        for (unsigned i = 0; i < SCREEN_ROTATE_PANEL_BYTES; ++i)
            dst[SCREEN_ROTATE_PANEL_BYTES - 1u - i] = screen_rotate_reverse8(src[i]);
        return;
    }
    if (r != SCREEN_ROTATE_90 && r != SCREEN_ROTATE_270) {
        memcpy(dst, src, SCREEN_ROTATE_PANEL_BYTES);
        return;
    }
    for (unsigned by = 0; by < BLOCKS; ++by)
        for (unsigned bx = 0; bx < BLOCKS; ++bx) {
            uint64_t x = 0;
            for (unsigned k = 0; k < 8; ++k) x |= (uint64_t)src[(by * 8u + k) * ROW + bx] << (8u * k);
            x = screen_rotate_flip_diagonal(x);
            /* 90: block (bx, by) goes to (BLOCKS-1-by, bx), rows reversed. 270: to (by, BLOCKS-1-bx), bits reversed. */
            const unsigned tx = r == SCREEN_ROTATE_90 ? BLOCKS - 1u - by : by;
            const unsigned ty = r == SCREEN_ROTATE_90 ? bx : BLOCKS - 1u - bx;
            for (unsigned j = 0; j < 8; ++j) {
                const uint8_t row = (uint8_t)(x >> (8u * (r == SCREEN_ROTATE_90 ? 7u - j : j)));
                dst[(ty * 8u + j) * ROW + tx] = r == SCREEN_ROTATE_90 ? row : screen_rotate_reverse8(row);
            }
        }
}

/* Rotates a packed game image (four pixels per byte, leftmost in the low two bits) of w x h pixels, both
 * multiples of four, into dst, which is dst_w pixels wide, already zero, and gets x_off blank columns on
 * its left. A zero pixel is white, so the blank columns match a white bezel. */
static inline void screen_rotate_packed(screen_rotation_t r, const uint8_t *src, unsigned w, unsigned h,
                                        uint8_t *dst, unsigned dst_w, unsigned x_off)
{
    const unsigned src_row = w / 4u, dst_row = dst_w / 4u;
    for (unsigned y = 0; y < h; ++y) {
        const uint8_t *row = src + y * src_row;
        for (unsigned x = 0; x < w; ++x) {
            const unsigned shade = (row[x >> 2] >> ((x & 3u) * 2u)) & 3u;
            if (!shade) continue;
            unsigned dx, dy;
            switch (r) {
            case SCREEN_ROTATE_90: dx = h - 1u - y; dy = x; break;
            case SCREEN_ROTATE_180: dx = w - 1u - x; dy = h - 1u - y; break;
            case SCREEN_ROTATE_270: dx = y; dy = w - 1u - x; break;
            default: dx = x; dy = y; break;
            }
            dx += x_off;
            dst[dy * dst_row + (dx >> 2)] |= (uint8_t)(shade << ((dx & 3u) * 2u));
        }
    }
}

/* Where a game rectangle lands on the rotated panel, exactly (not yet widened to a multiple of four). */
static inline void screen_rotate_rect(screen_rotation_t r, game_view_t v, unsigned *x, unsigned *y, unsigned *w, unsigned *h)
{
    const unsigned side = SCREEN_ROTATE_SIDE;
    switch (r) {
    case SCREEN_ROTATE_90: *x = side - v.y - v.height; *y = v.x; *w = v.height; *h = v.width; break;
    case SCREEN_ROTATE_180: *x = side - v.x - v.width; *y = side - v.y - v.height; *w = v.width; *h = v.height; break;
    case SCREEN_ROTATE_270: *x = v.y; *y = side - v.x - v.width; *w = v.height; *h = v.width; break;
    default: *x = v.x; *y = v.y; *w = v.width; *h = v.height; break;
    }
}

/* Turns a game image and its view for the rotated panel. The planner wants x and width in multiples of
 * four, so the rectangle is widened to the next multiple and the extra columns are white. dst must hold
 * SCREEN_ROTATE_GAME_BYTES. Returns the view of the image written to dst. */
static inline game_view_t screen_rotate_game(screen_rotation_t r, const uint8_t *src, game_view_t v, uint8_t *dst)
{
    unsigned x, y, w, h;
    screen_rotate_rect(r, v, &x, &y, &w, &h);
    const unsigned aligned_x = x & ~3u, aligned_w = ((x + w - aligned_x) + 3u) & ~3u;
    memset(dst, 0, aligned_w * h / 4u);
    screen_rotate_packed(r, src, v.width, v.height, dst, aligned_w, x - aligned_x);
    return (game_view_t){(uint8_t)aligned_x, (uint8_t)y, (uint8_t)aligned_w, (uint8_t)h};
}
