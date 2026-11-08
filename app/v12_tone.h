#ifndef BTA_V12_TONE_H
#define BTA_V12_TONE_H
#include "bta_types.h"
/* V12: fixed-clock, non-bursting test-tone sender.
 * V11 set next=actual_send+4ms, which added every scheduling overshoot to
 * the audio clock. At 750us polling this can silently reduce the effective
 * packet rate to ~222/s rather than 250/s. Keep a 4ms timebase anchored to
 * the first packet, advancing past deadlines missed after a real stall.
 * We deliberately do NOT send extra back-to-back packets: V8's catchup mode
 * remains disabled for the capture path, and the Bluetooth device may reject
 * aggressive bursts. This helper affects only the synthetic test tone.
 * Counters are read-only diagnostics; no extra PS4 driver calls. */
#define BTA_V12_TONE_US 4000UL
struct bta_v12_tone {
    u64 due, last_at;
    u32 packets, skipped, late4, late8, maxlate_us;
    u32 gaps8, maxgap_us, ioctl4, ioctl8, maxcall_us;
    u32 refused, failed;
};
static void bta_v12_tone_reset(struct bta_v12_tone *t) {
    *t = (struct bta_v12_tone){0};
}
static int bta_v12_tone_ready(struct bta_v12_tone *t, u64 now) {
    if (!t->due) { /* first packet immediately */
        t->due = now + BTA_V12_TONE_US;
    } else {
        if (now < t->due) return 0;
        u64 lag = now - t->due;
        if (lag > 4000) t->late4++;
        if (lag > 8000) t->late8++;
        if (lag > t->maxlate_us) t->maxlate_us = (u32)(lag > 0xffffffffUL ? 0xffffffffUL : lag);
        /* A slow syscall or descheduled thread can miss a whole 4ms slot.
         * Skip missing deadlines rather than queueing a burst of old tone. */
        u64 missed = lag / BTA_V12_TONE_US;
        if (missed > 0xffffffffUL - t->skipped) t->skipped = 0xffffffffu;
        else t->skipped += (u32)missed;
        t->due += (missed + 1) * BTA_V12_TONE_US;
    }
    if (t->last_at && now >= t->last_at) {
        u64 gap = now - t->last_at;
        if (gap > 8000) t->gaps8++;
        if (gap > t->maxgap_us) t->maxgap_us = (u32)(gap > 0xffffffffUL ? 0xffffffffUL : gap);
    }
    t->last_at = now;
    t->packets++;
    return 1;
}
static void bta_v12_tone_note_result(struct bta_v12_tone *t, u64 start, u64 end, long rc) {
    if (rc == 0) t->refused++;
    else if (rc < 0) t->failed++;
    if (end >= start) {
        u64 duration = end - start;
        if (duration > 4000) t->ioctl4++;
        if (duration > 8000) t->ioctl8++;
        if (duration > t->maxcall_us) t->maxcall_us = (u32)(duration > 0xffffffffUL ? 0xffffffffUL : duration);
    }
}
#endif
