#ifndef BTA_TIMING_H
#define BTA_TIMING_H
#include "bta_types.h"
#include "json.h"
/* Per-packet send timing, summarised one row per second and reported at
 * GET /api/timing. Kept pure (no clock calls) so a PC test can drive the ring
 * with a fake clock. gap: time between two sends (4 ms is on time). send: how
 * long the driver call took. backlog: capture audio waiting to be sent.
 * refused: the driver returned 0. failed: it returned an error. */
struct bta_second {
    u32 seq, t, pkts, refused, failed, late8, late16, late50, maxgap_us, maxsend_us, maxback_ms, peak;
};

/* Close the current second and open a new one once `now` has advanced past it.
 * A second with no packets and no gap is dropped rather than recorded. */
static void bta_roll_second(struct bta_second *secs, struct bta_second *cur,
                            u32 *sec_seq, u64 *sec_start, u64 now) {
    if (*sec_start && now - *sec_start < 1000000) return;
    if (*sec_start && (cur->pkts || cur->maxgap_us)) {
        cur->seq = ++*sec_seq;
        secs[*sec_seq % 128] = *cur;
    }
    *cur = (struct bta_second){0};
    cur->t = (u32)(now / 1000000);
    *sec_start = now;
}

/* Oldest first: {"seq",...,"secs":[[seq,t,pkts,refused,failed,late8,late16,
 * late50,maxgap_us,maxsend_us,maxback_ms,peak], ...]} for the last two minutes. */
static unsigned bta_timing_json(const struct bta_second *secs, u32 sec_seq, long now_s,
                                char *out, unsigned cap) {
    struct writer w = {out, 0, cap};
    append(&w, "{\"seq\":");
    number(&w, sec_seq);
    field(&w, "now_s", now_s);
    append(&w, ",\"secs\":[");
    u32 n = sec_seq < 120 ? sec_seq : 120;
    for (u32 i = 0; i < n; i++) {
        const struct bta_second *s = &secs[(sec_seq - n + 1 + i) % 128];
        if (i) append(&w, ",");
        u32 v[12] = {s->seq, s->t, s->pkts, s->refused, s->failed, s->late8,
                     s->late16, s->late50, s->maxgap_us, s->maxsend_us, s->maxback_ms, s->peak};
        append(&w, "[");
        for (int k = 0; k < 12; k++) { if (k) append(&w, ","); number(&w, v[k]); }
        append(&w, "]");
    }
    append(&w, "]}");
    return w.used;
}
#endif
