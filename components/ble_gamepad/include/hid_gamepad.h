#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Transport-independent HID gamepad decoder. It reads a HID report descriptor once and then turns
 * input reports into one normalized bit set, so the rest of the player never sees device layouts.
 *
 * Normalized controls: bit 0..3 = D-pad Up, Down, Left, Right (from a hat switch, D-pad usages or
 * the left stick), bit 3+N = HID button N (N = 1..28). Which physical button is "A" differs between
 * pads, so the player maps these controls to Game Boy buttons with its own wizard. */
enum { HID_CTRL_UP, HID_CTRL_DOWN, HID_CTRL_LEFT, HID_CTRL_RIGHT, HID_CTRL_BUTTON_1 };
#define HID_PAD_BUTTONS 28u
#define HID_PAD_LAYOUTS 4u
#define HID_PAD_NONE 0xffffu
/* The left stick counts as a direction beyond this share of its travel from either end. */
#define HID_PAD_STICK_PERCENT 25

typedef struct { uint16_t offset; uint8_t size; int32_t min, max; } hid_field_t; /* size 0: absent */

typedef struct {
    uint8_t report_id;
    hid_field_t hat, x, y;
    uint16_t dpad[4];                 /* bit offsets of D-pad Up, Down, Left, Right usages */
    uint16_t button[HID_PAD_BUTTONS]; /* bit offset of button N+1, HID_PAD_NONE if absent */
} hid_layout_t;

typedef struct {
    unsigned count;
    hid_layout_t layout[HID_PAD_LAYOUTS];
    uint32_t controls; /* normalized controls this pad can report */
} hid_pad_t;

/* Parses the input fields of the first Game Pad / Joystick application collections. False when the
 * descriptor has none or no buttons; any input, however malformed, is safe. */
bool hid_pad_parse(const uint8_t *map, size_t length, hid_pad_t *pad);
/* `data` excludes the report ID byte. *known is false for report IDs the pad does not use, and the
 * caller should then keep its previous state. */
uint32_t hid_pad_decode(const hid_pad_t *pad, unsigned report_id, const uint8_t *data, size_t length, bool *known);
