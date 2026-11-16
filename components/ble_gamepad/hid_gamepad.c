#include "hid_gamepad.h"
#include <limits.h>
#include <string.h>

#define MAX_USAGES 32u
#define MAX_REPORT_IDS 8u
#define STACK_DEPTH 4u
#define MAX_BITS 0xfffffu
#define MAX_FIELDS 256u
#define USAGE_GAMEPAD 0x00010005u
#define USAGE_JOYSTICK 0x00010004u

typedef struct {
    uint32_t usage_page;
    int32_t logical_min;
    uint32_t logical_max_raw;
    unsigned logical_max_bytes, report_size, report_count, report_id;
} globals_t;

typedef struct {
    hid_pad_t *pad;
    globals_t g, stack[STACK_DEPTH];
    unsigned stack_depth;
    uint32_t usage[MAX_USAGES], usage_min, usage_max;
    unsigned usages;
    bool has_min, has_range;
    unsigned collection_depth;
    bool in_pad;
    unsigned ids[MAX_REPORT_IDS];
    uint32_t bits[MAX_REPORT_IDS];
    unsigned id_count;
} parser_t;

static int32_t sign_extend_bytes(uint32_t v, unsigned bytes)
{
    if (bytes == 1) return (int8_t)v;
    if (bytes == 2) return (int16_t)v;
    return (int32_t)v;
}

static int32_t resolve_max(const globals_t *g)
{
    int32_t max = sign_extend_bytes(g->logical_max_raw, g->logical_max_bytes);
    /* A non-negative minimum makes the maximum unsigned: 0..65535 is written as 0x0000FFFF or 0xFFFF. */
    if (g->logical_min >= 0 && max < g->logical_min)
        max = g->logical_max_raw > (uint32_t)INT32_MAX ? INT32_MAX : (int32_t)g->logical_max_raw;
    return max;
}

static uint32_t *counter_for(parser_t *p, unsigned id)
{
    for (unsigned i = 0; i < p->id_count; ++i) if (p->ids[i] == id) return &p->bits[i];
    if (p->id_count == MAX_REPORT_IDS) return NULL;
    p->ids[p->id_count] = id;
    p->bits[p->id_count] = 0;
    return &p->bits[p->id_count++];
}

static hid_layout_t *layout_for(hid_pad_t *pad, unsigned id)
{
    for (unsigned i = 0; i < pad->count; ++i) if (pad->layout[i].report_id == id) return &pad->layout[i];
    if (pad->count == HID_PAD_LAYOUTS) return NULL;
    hid_layout_t *l = &pad->layout[pad->count++];
    memset(l, 0, sizeof(*l));
    l->report_id = (uint8_t)id;
    for (unsigned i = 0; i < 4; ++i) l->dpad[i] = HID_PAD_NONE;
    for (unsigned i = 0; i < HID_PAD_BUTTONS; ++i) l->button[i] = HID_PAD_NONE;
    return l;
}

static void record(parser_t *p, uint32_t usage, uint32_t offset)
{
    const unsigned page = usage >> 16, id = usage & 0xffffu, size = p->g.report_size;
    const bool button = page == 9 && id >= 1 && id <= HID_PAD_BUTTONS;
    const bool dpad = page == 1 && id >= 0x90 && id <= 0x93;
    const bool ranged = page == 1 && (id == 0x39 || id == 0x30 || id == 0x31);
    if (offset >= HID_PAD_NONE || !(button || dpad || ranged)) return;
    if ((button || dpad) && size != 1) return;
    if (ranged && size > 32) return;
    hid_layout_t *l = layout_for(p->pad, p->g.report_id);
    if (!l) return;
    if (button) { l->button[id - 1] = (uint16_t)offset; return; }
    if (dpad) {
        static const uint8_t slot[4] = {0, 1, 3, 2}; /* 0x90 up, 0x91 down, 0x92 right, 0x93 left */
        l->dpad[slot[id - 0x90]] = (uint16_t)offset;
        return;
    }
    hid_field_t *f = id == 0x39 ? &l->hat : id == 0x30 ? &l->x : &l->y;
    *f = (hid_field_t){(uint16_t)offset, (uint8_t)size, p->g.logical_min, resolve_max(&p->g)};
}

static bool nth_usage(const parser_t *p, unsigned i, uint32_t *usage)
{
    if (p->has_range) {
        if (p->usage_max < p->usage_min) return false;
        const uint32_t span = p->usage_max - p->usage_min;
        *usage = p->usage_min + (i <= span ? i : span);
        return true;
    }
    if (!p->usages) return false;
    *usage = p->usage[i < p->usages ? i : p->usages - 1];
    return true;
}

static void input_item(parser_t *p, uint32_t flags)
{
    uint32_t *counter = counter_for(p, p->g.report_id);
    const unsigned size = p->g.report_size, count = p->g.report_count;
    if (!counter) return;
    const uint32_t start = *counter;
    uint64_t end = (uint64_t)start + (uint64_t)size * count;
    *counter = end > MAX_BITS ? MAX_BITS : (uint32_t)end;
    /* Constant padding and key-code arrays carry no gamepad controls. */
    if ((flags & 1u) || !(flags & 2u) || !p->in_pad || !size) return;
    for (unsigned i = 0; i < count && i < MAX_FIELDS; ++i) {
        uint32_t usage;
        if (nth_usage(p, i, &usage)) record(p, usage, start + i * size);
    }
}

static void main_item(parser_t *p, unsigned tag, uint32_t value)
{
    if (tag == 8) input_item(p, value);
    else if (tag == 10) {
        if (!p->collection_depth && value == 1 && p->usages &&
            (p->usage[0] == USAGE_GAMEPAD || p->usage[0] == USAGE_JOYSTICK)) p->in_pad = true;
        ++p->collection_depth;
    } else if (tag == 12 && p->collection_depth && --p->collection_depth == 0) p->in_pad = false;
    p->usages = 0;
    p->has_min = p->has_range = false;
}

static void global_item(parser_t *p, unsigned tag, uint32_t value, unsigned size)
{
    globals_t *g = &p->g;
    switch (tag) {
    case 0: g->usage_page = value; break;
    case 1: g->logical_min = sign_extend_bytes(value, size); break;
    case 2: g->logical_max_raw = value; g->logical_max_bytes = size; break;
    case 7: g->report_size = value; break;
    case 8: g->report_id = value & 0xffu; break;
    case 9: g->report_count = value; break;
    case 10: if (p->stack_depth < STACK_DEPTH) p->stack[p->stack_depth++] = *g; break;
    case 11: if (p->stack_depth) *g = p->stack[--p->stack_depth]; break;
    default: break;
    }
}

static void local_item(parser_t *p, unsigned tag, uint32_t value, unsigned size)
{
    /* Like most parsers, short usages take the Usage Page in effect when the usage is declared. */
    const uint32_t usage = size == 4 ? value : (p->g.usage_page << 16) | (value & 0xffffu);
    if (tag == 0) { if (p->usages < MAX_USAGES) p->usage[p->usages++] = usage; }
    else if (tag == 1) { p->usage_min = usage; p->has_min = true; }
    else if (tag == 2) { p->usage_max = usage; p->has_range = p->has_min; }
}

bool hid_pad_parse(const uint8_t *map, size_t length, hid_pad_t *pad)
{
    if (!pad) return false;
    memset(pad, 0, sizeof(*pad));
    if (!map) return false;
    parser_t p;
    memset(&p, 0, sizeof(p));
    p.pad = pad;
    for (size_t i = 0; i < length;) {
        const uint8_t prefix = map[i++];
        if (prefix == 0xfe) { /* Long item: data size, long tag, data. Unused by real gamepads. */
            if (i >= length) break;
            i += 2u + map[i];
            continue;
        }
        unsigned size = prefix & 3u;
        if (size == 3) size = 4;
        if (i + size > length) break;
        uint32_t value = 0;
        for (unsigned k = 0; k < size; ++k) value |= (uint32_t)map[i + k] << (8 * k);
        i += size;
        const unsigned type = (prefix >> 2) & 3u, tag = prefix >> 4;
        if (type == 0) main_item(&p, tag, value);
        else if (type == 1) global_item(&p, tag, value, size);
        else if (type == 2) local_item(&p, tag, value, size);
    }
    for (unsigned i = 0; i < pad->count; ++i) {
        const hid_layout_t *l = &pad->layout[i];
        if (l->hat.size) pad->controls |= 0xfu;
        if (l->x.size) pad->controls |= 1u << HID_CTRL_LEFT | 1u << HID_CTRL_RIGHT;
        if (l->y.size) pad->controls |= 1u << HID_CTRL_UP | 1u << HID_CTRL_DOWN;
        for (unsigned d = 0; d < 4; ++d) if (l->dpad[d] != HID_PAD_NONE) pad->controls |= 1u << d;
        for (unsigned b = 0; b < HID_PAD_BUTTONS; ++b)
            if (l->button[b] != HID_PAD_NONE) pad->controls |= 1u << (HID_CTRL_BUTTON_1 + b);
    }
    if (!(pad->controls >> HID_CTRL_BUTTON_1)) { /* A pad without a single button cannot play. */
        memset(pad, 0, sizeof(*pad));
        return false;
    }
    return true;
}

static bool get_bits(const uint8_t *data, size_t length, unsigned offset, unsigned size, uint32_t *out)
{
    if (!size || size > 32 || (size_t)offset + size > length * 8) return false;
    uint32_t v = 0;
    for (unsigned i = 0; i < size; ++i) {
        const unsigned bit = offset + i;
        v |= (uint32_t)((data[bit >> 3] >> (bit & 7u)) & 1u) << i;
    }
    *out = v;
    return true;
}

static bool field_value(const uint8_t *data, size_t length, const hid_field_t *f, int32_t *out)
{
    uint32_t raw;
    if (!f->size || !get_bits(data, length, f->offset, f->size, &raw)) return false;
    if (f->min < 0) {
        const uint32_t m = 1u << (f->size - 1u);
        *out = (int32_t)((raw ^ m) - m);
    } else *out = raw > (uint32_t)INT32_MAX ? INT32_MAX : (int32_t)raw;
    return true;
}

static uint32_t hat_controls(const hid_field_t *f, int32_t v)
{
    enum { U = 1u << HID_CTRL_UP, D = 1u << HID_CTRL_DOWN, L = 1u << HID_CTRL_LEFT, R = 1u << HID_CTRL_RIGHT };
    static const uint8_t eight[8] = {U, U | R, R, D | R, D, D | L, L, U | L};
    if (v < f->min || v > f->max) return 0; /* Outside the logical range is the neutral "null state". */
    const int64_t span = (int64_t)f->max - f->min + 1, index = (int64_t)v - f->min;
    if (span == 8) return eight[index];
    if (span == 4) return eight[index * 2];
    return 0;
}

static uint32_t stick_controls(const hid_field_t *f, int32_t v, uint32_t low, uint32_t high)
{
    const int64_t range = (int64_t)f->max - f->min;
    if (range <= 0 || v < f->min || v > f->max) return 0;
    const int64_t percent = ((int64_t)v - f->min) * 100 / range;
    return percent < HID_PAD_STICK_PERCENT ? low : percent > 100 - HID_PAD_STICK_PERCENT ? high : 0;
}

uint32_t hid_pad_decode(const hid_pad_t *pad, unsigned report_id, const uint8_t *data, size_t length, bool *known)
{
    uint32_t out = 0;
    if (known) *known = false;
    if (!pad || !data) return 0;
    const hid_layout_t *l = NULL;
    for (unsigned i = 0; i < pad->count; ++i) if (pad->layout[i].report_id == report_id) l = &pad->layout[i];
    if (!l) return 0;
    if (known) *known = true;
    uint32_t bit;
    for (unsigned n = 0; n < HID_PAD_BUTTONS; ++n)
        if (l->button[n] != HID_PAD_NONE && get_bits(data, length, l->button[n], 1, &bit) && bit)
            out |= 1u << (HID_CTRL_BUTTON_1 + n);
    for (unsigned d = 0; d < 4; ++d)
        if (l->dpad[d] != HID_PAD_NONE && get_bits(data, length, l->dpad[d], 1, &bit) && bit) out |= 1u << d;
    int32_t v;
    if (field_value(data, length, &l->hat, &v)) out |= hat_controls(&l->hat, v);
    /* HID Y grows downward on every pad that follows the usage tables. */
    if (field_value(data, length, &l->x, &v))
        out |= stick_controls(&l->x, v, 1u << HID_CTRL_LEFT, 1u << HID_CTRL_RIGHT);
    if (field_value(data, length, &l->y, &v))
        out |= stick_controls(&l->y, v, 1u << HID_CTRL_UP, 1u << HID_CTRL_DOWN);
    return out;
}
