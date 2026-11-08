#ifndef BTA_V13_SENDER_H
#define BTA_V13_SENDER_H
#include "bta_types.h"
/* Five-second sender diagnostics. Record scheduler wake slips separately from
 * mutex waits. The audio timestamp must be sampled AFTER acquiring the lock;
 * otherwise any contention makes the time supplied to both pacers stale. */
struct bta_v13_sender {
    u32 loops, lock1, lock4, lock8, maxlock_us;
    u32 sleep4, sleep8, maxsleep_us;
};
static void bta_v13_note(struct bta_v13_sender *s, u64 lock_start,
                        u64 lock_acquired, u64 sleep_start, u32 poll_us) {
    s->loops++;
    if (lock_acquired >= lock_start) {
        u64 d = lock_acquired - lock_start;
        if (d > 1000) s->lock1++;
        if (d > 4000) s->lock4++;
        if (d > 8000) s->lock8++;
        if (d > s->maxlock_us) s->maxlock_us = (u32)(d > 0xffffffffUL ? 0xffffffffUL : d);
    }
    /* Overshoot beyond the requested sleep (not a total sender-loop gap). */
    if (sleep_start && lock_start > sleep_start + poll_us) {
        u64 d = lock_start - (sleep_start + poll_us);
        if (d > 4000) s->sleep4++;
        if (d > 8000) s->sleep8++;
        if (d > s->maxsleep_us) s->maxsleep_us = (u32)(d > 0xffffffffUL ? 0xffffffffUL : d);
    }
}
static void bta_v13_reset(struct bta_v13_sender *s) {
    *s = (struct bta_v13_sender){0};
}
#endif
