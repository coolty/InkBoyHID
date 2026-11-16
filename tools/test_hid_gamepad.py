#!/usr/bin/env python3
"""Parse real-world style HID report descriptors and decode input reports into normalized controls."""
from host_build import ROOT, run

CODE = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "hid_gamepad.h"

#define BIT(n) (1u << (n))
#define UP BIT(HID_CTRL_UP)
#define DOWN BIT(HID_CTRL_DOWN)
#define LEFT BIT(HID_CTRL_LEFT)
#define RIGHT BIT(HID_CTRL_RIGHT)
#define BTN(n) BIT(HID_CTRL_BUTTON_1 + (n) - 1)

/* Xbox-style: report 1, 16-bit sticks, 10-bit triggers, 4-bit hat 1..8, 15 buttons. */
static const uint8_t xbox[] = {
    0x05,0x01, 0x09,0x05, 0xA1,0x01, 0x85,0x01,
    0x09,0x01, 0xA1,0x00, 0x09,0x30, 0x09,0x31, 0x15,0x00, 0x27,0xFF,0xFF,0x00,0x00, 0x95,0x02, 0x75,0x10, 0x81,0x02, 0xC0,
    0x09,0x01, 0xA1,0x00, 0x09,0x32, 0x09,0x35, 0x15,0x00, 0x27,0xFF,0xFF,0x00,0x00, 0x95,0x02, 0x75,0x10, 0x81,0x02, 0xC0,
    0x05,0x02, 0x09,0xC5, 0x15,0x00, 0x26,0xFF,0x03, 0x95,0x01, 0x75,0x0A, 0x81,0x02,
    0x15,0x00, 0x25,0x00, 0x75,0x06, 0x95,0x01, 0x81,0x03,
    0x05,0x02, 0x09,0xC4, 0x15,0x00, 0x26,0xFF,0x03, 0x95,0x01, 0x75,0x0A, 0x81,0x02,
    0x15,0x00, 0x25,0x00, 0x75,0x06, 0x95,0x01, 0x81,0x03,
    0x05,0x01, 0x09,0x39, 0x15,0x01, 0x25,0x08, 0x35,0x00, 0x46,0x3B,0x01, 0x66,0x14,0x00, 0x75,0x04, 0x95,0x01, 0x81,0x42,
    0x75,0x04, 0x95,0x01, 0x15,0x00, 0x25,0x00, 0x35,0x00, 0x45,0x00, 0x81,0x03,
    0x05,0x09, 0x19,0x01, 0x29,0x0F, 0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x0F, 0x81,0x02,
    0x15,0x00, 0x25,0x00, 0x75,0x01, 0x95,0x01, 0x81,0x03,
    0xC0,
};
/* Joystick without report IDs: 8 buttons, then signed 8-bit X and Y. */
static const uint8_t joystick[] = {
    0x05,0x01, 0x09,0x04, 0xA1,0x01, 0x05,0x09, 0x19,0x01, 0x29,0x08, 0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x08, 0x81,0x02,
    0x05,0x01, 0x09,0x30, 0x09,0x31, 0x15,0x81, 0x25,0x7F, 0x75,0x08, 0x95,0x02, 0x81,0x02, 0xC0,
};
/* D-pad as four Generic Desktop usages (listed up, down, left, right) and 4 buttons. */
static const uint8_t dpad_usages[] = {
    0x05,0x01, 0x09,0x05, 0xA1,0x01, 0x09,0x90, 0x09,0x91, 0x09,0x93, 0x09,0x92, 0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x04, 0x81,0x02,
    0x75,0x04, 0x95,0x01, 0x81,0x03, 0x05,0x09, 0x19,0x01, 0x29,0x04, 0x95,0x04, 0x75,0x01, 0x81,0x02, 0x75,0x04, 0x95,0x01, 0x81,0x03, 0xC0,
};
static const uint8_t keyboard[] = {
    0x05,0x01, 0x09,0x06, 0xA1,0x01, 0x05,0x07, 0x19,0xE0, 0x29,0xE7, 0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x08, 0x81,0x02, 0xC0,
};

static void put_bits(uint8_t *d, unsigned offset, unsigned size, uint32_t value)
{
    for (unsigned i = 0; i < size; ++i) {
        const unsigned b = offset + i;
        if ((value >> i) & 1u) d[b / 8] |= (uint8_t)(1u << (b % 8)); else d[b / 8] &= (uint8_t)~(1u << (b % 8));
    }
}

static void xbox_layout(void)
{
    hid_pad_t pad;
    assert(hid_pad_parse(xbox, sizeof(xbox), &pad) && pad.count == 1 && pad.layout[0].report_id == 1);
    assert(pad.layout[0].x.offset == 0 && pad.layout[0].y.offset == 16 && pad.layout[0].x.max == 65535);
    assert(pad.layout[0].hat.offset == 96 && pad.layout[0].hat.size == 4 && pad.layout[0].hat.min == 1 && pad.layout[0].hat.max == 8);
    for (unsigned n = 0; n < 15; ++n) assert(pad.layout[0].button[n] == 104 + n);
    assert(pad.layout[0].button[15] == HID_PAD_NONE);
    assert(pad.controls == (UP | DOWN | LEFT | RIGHT | (BTN(15) | (BTN(15) - BTN(1)))));
    uint8_t report[15];
    memset(report, 0, sizeof(report));
    put_bits(report, 0, 16, 0x8000); put_bits(report, 16, 16, 0x8000);
    bool known = false;
    assert(hid_pad_decode(&pad, 1, report, sizeof(report), &known) == 0 && known); /* Centered, hat in its null state. */
    put_bits(report, 96, 4, 3); put_bits(report, 104, 1, 1); put_bits(report, 115, 1, 1);
    assert(hid_pad_decode(&pad, 1, report, sizeof(report), &known) == (RIGHT | BTN(1) | BTN(12)));
    put_bits(report, 96, 4, 8); /* North-west. */
    assert(hid_pad_decode(&pad, 1, report, sizeof(report), &known) == (UP | LEFT | BTN(1) | BTN(12)));
    put_bits(report, 96, 4, 0); put_bits(report, 0, 16, 0); put_bits(report, 16, 16, 0xffff); /* Stick far left and down. */
    assert(hid_pad_decode(&pad, 1, report, sizeof(report), &known) == (LEFT | DOWN | BTN(1) | BTN(12)));
    put_bits(report, 0, 16, 0x8000 + 0x2000); /* Within the dead zone. */
    put_bits(report, 16, 16, 0x8000 - 0x2000);
    assert(!(hid_pad_decode(&pad, 1, report, sizeof(report), &known) & (LEFT | RIGHT | UP | DOWN)));
    assert(hid_pad_decode(&pad, 2, report, sizeof(report), &known) == 0 && !known); /* Other reports leave state alone. */
    assert(hid_pad_decode(&pad, 1, report, 3, &known) == 0 && known); /* Truncated: nothing out of range is read. */
}

static void joystick_and_dpad(void)
{
    hid_pad_t pad;
    bool known;
    assert(hid_pad_parse(joystick, sizeof(joystick), &pad) && pad.layout[0].report_id == 0);
    const uint8_t report[3] = {0x81, 0x81, 0x7f};
    assert(hid_pad_decode(&pad, 0, report, 3, &known) == (BTN(1) | BTN(8) | LEFT | DOWN) && known);
    assert(hid_pad_parse(dpad_usages, sizeof(dpad_usages), &pad));
    const uint8_t buttons[2] = {0x05, 0x02};
    assert(hid_pad_decode(&pad, 0, buttons, 2, &known) == (UP | LEFT | BTN(2)));
}

static void rejects_and_survives_garbage(void)
{
    hid_pad_t pad;
    assert(!hid_pad_parse(keyboard, sizeof(keyboard), &pad) && !pad.count && !pad.controls);
    assert(!hid_pad_parse(NULL, 0, &pad) && !hid_pad_parse(xbox, 0, &pad) && !hid_pad_parse(xbox, sizeof(xbox), NULL));
    for (size_t cut = 0; cut < sizeof(xbox); ++cut) (void)hid_pad_parse(xbox, cut, &pad); /* Every truncation. */
    uint8_t noise[256];
    uint32_t seed = 7;
    for (unsigned round = 0; round < 20000; ++round) {
        for (unsigned i = 0; i < sizeof(noise); ++i) { seed = seed * 1664525u + 1013904223u; noise[i] = (uint8_t)(seed >> 24); }
        size_t length = 1 + (seed >> 8) % sizeof(noise);
        if (round % 2) { memcpy(noise, xbox, sizeof(xbox) < length ? sizeof(xbox) : length); noise[(seed >> 3) % length] ^= (uint8_t)seed; }
        if (hid_pad_parse(noise, length, &pad)) {
            uint8_t report[40];
            bool known;
            memset(report, (int)seed, sizeof(report));
            for (unsigned id = 0; id < 4; ++id) (void)hid_pad_decode(&pad, id, report, sizeof(report), &known);
        }
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "xbox")) xbox_layout();
    else if (!strcmp(argv[1], "others")) joystick_and_dpad();
    else if (!strcmp(argv[1], "garbage")) rejects_and_survives_garbage();
    else assert(0);
    return 0;
}
'''

run(CODE, extra_sources=[ROOT / 'components/ble_gamepad/hid_gamepad.c'], scenarios=[[x] for x in ('xbox', 'others', 'garbage')])
print('PASS: Xbox-style, joystick and D-pad-usage descriptors decode to normalized controls; keyboards are rejected; truncated and random descriptors are safe')
