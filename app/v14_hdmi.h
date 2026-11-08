#ifndef BTA_V14_HDMI_H
#define BTA_V14_HDMI_H
#include "bta_types.h"
/* The recurring HDMI mute keepalive should never block the audio sender.
 * The worker owns the HDMI fd for its full lifetime; it closes only after
 * stopping the sender. The once-per-second predicate is snapped under mutex. */
struct bta_v14_hdmi { u32 calls, slow4, slow8, maxcall_us, errors; };
static int bta_v14_mute_needed(int streaming, int muted, long hdmi_fd) {
    return streaming && muted == 1 && hdmi_fd >= 0;
}
static void bta_v14_hdmi_note(struct bta_v14_hdmi *d, u64 start, u64 end, long rc) {
    d->calls++;
    if (rc) d->errors++;
    if (end >= start) {
        u64 us = end - start;
        if (us > 4000) d->slow4++;
        if (us > 8000) d->slow8++;
        if (us > d->maxcall_us) d->maxcall_us = (u32)(us > 0xffffffffUL ? 0xffffffffUL : us);
    }
}
static void bta_v14_hdmi_reset(struct bta_v14_hdmi *d) {
    *d = (struct bta_v14_hdmi){0};
}
#endif
