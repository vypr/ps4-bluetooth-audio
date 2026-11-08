#ifndef BTA_DIAG_V6_H
#define BTA_DIAG_V6_H
#include "bta_types.h"
/* FW14 diagnostic-only accounting. Called after the existing BT_SEND_AUDIO
 * ioctl; never polls /dev/bt, touches the audio payload, or writes files.
 * All fields are protected by worker.c's existing sender/state mutex. */
struct bta_diag6_window {
    u32 game_packets, tone_packets, game_nonzero, tone_nonzero;
    u32 rc_one, rc_bytes, rc_other, refused, failed;
    u32 max_send_us, max_gap_us, game_peak, tone_peak;
    long last_rc;
    u64 last_send_at;
};
static void bta_diag6_note(struct bta_diag6_window *d, int tone, long rc,
                            unsigned peak, u64 now, u64 done) {
    if (tone) {
        d->tone_packets++;
        if (peak) d->tone_nonzero++;
        if (peak > d->tone_peak) d->tone_peak = peak;
    } else {
        d->game_packets++;
        if (peak) d->game_nonzero++;
        if (peak > d->game_peak) d->game_peak = peak;
    }
    if (rc == 1) d->rc_one++;
    else if (rc == 768) d->rc_bytes++;
    else if (rc > 0) d->rc_other++;
    else if (rc == 0) d->refused++;
    else d->failed++;
    d->last_rc = rc;
    if (done >= now && done - now > d->max_send_us)
        d->max_send_us = (u32)((done - now) > 0xffffffffUL ? 0xffffffffUL : done - now);
    if (d->last_send_at && now >= d->last_send_at && now - d->last_send_at > d->max_gap_us)
        d->max_gap_us = (u32)((now - d->last_send_at) > 0xffffffffUL ? 0xffffffffUL : now - d->last_send_at);
    d->last_send_at = now;
}
static void bta_diag6_reset(struct bta_diag6_window *d) {
    *d = (struct bta_diag6_window){0};
}
#endif
