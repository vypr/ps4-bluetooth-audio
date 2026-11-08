#ifndef BTA_V8_DIAG_H
#define BTA_V8_DIAG_H
#include "bta_types.h"
/* Read-only counters. No IO and no additional Bluetooth or kernel calls. */
struct bta_v8_diag {
    u64 trim_bytes;
    u32 trim_events, max_trim_bytes, underruns, resync_events;
    u32 late4, late8, late16, maxlate_us;
    u32 calls4, calls8, maxcall_us, catchups, gain_limited;
};
static void bta_v8_diag_reset(struct bta_v8_diag *d) {
    *d = (struct bta_v8_diag){0};
}
static void bta_v8_note_pacer(struct bta_v8_diag *d, int kind, u32 skipped,
                               int was_filling, u64 now, u64 due, int catchup) {
    if (kind == 3) d->underruns++;
    if (kind == 2) d->resync_events++;
    if (skipped) {
        d->trim_events++;
        d->trim_bytes += skipped;
        if (skipped > d->max_trim_bytes) d->max_trim_bytes = skipped;
    }
    if (catchup) d->catchups++;
    if (!was_filling && (kind == 1 || kind == 2) && now > due) {
        u64 late = now - due;
        if (late > 4000) d->late4++;
        if (late > 8000) d->late8++;
        if (late > 16000) d->late16++;
        if (late > d->maxlate_us)
            d->maxlate_us = (u32)(late > 0xffffffffUL ? 0xffffffffUL : late);
    }
}
static void bta_v8_note_call(struct bta_v8_diag *d, u64 start, u64 finish) {
    if (finish < start) return;
    u64 delay = finish - start;
    if (delay > 4000) d->calls4++;
    if (delay > 8000) d->calls8++;
    if (delay > d->maxcall_us)
        d->maxcall_us = (u32)(delay > 0xffffffffUL ? 0xffffffffUL : delay);
}
#endif
