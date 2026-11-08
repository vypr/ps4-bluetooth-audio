#ifndef BTA_V9_TUNE_H
#define BTA_V9_TUNE_H
#include "bta_types.h"
/* Session-only, runtime audio-tuning controls. Keep PCM at 48kHz and 192
 * stereo frames per 4ms packet. Sender sleep affects timing precision, not
 * the audio clock. Cushion and margin affect CAPTURE only, never Test Sound.
 * Each setter is pure so the host harness can exercise all bounds. */
struct bta_v9_tune { u32 cushion_ms, margin_ms, poll_us; };
enum { BTA_V9_CUSHION = 0, BTA_V9_MARGIN = 1, BTA_V9_POLL = 2 };
static void bta_v9_tune_init(struct bta_v9_tune *t, u32 latency_cushion_ms) {
    t->cushion_ms = latency_cushion_ms; t->margin_ms = 0; t->poll_us = 500;
}
static int bta_v9_tune_set(struct bta_v9_tune *t, int knob, u32 value) {
    if (knob == BTA_V9_CUSHION && value >= 8 && value <= 32 && value % 4 == 0) {
        t->cushion_ms = value; return 1;
    }
    if (knob == BTA_V9_MARGIN && value <= 16 && value % 4 == 0) {
        t->margin_ms = value; return 1;
    }
    if (knob == BTA_V9_POLL && value >= 250 && value <= 1000 && value % 250 == 0) {
        t->poll_us = value; return 1;
    }
    return 0;
}
#endif
