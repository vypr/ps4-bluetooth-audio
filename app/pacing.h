#ifndef BTA_PACING_H
#define BTA_PACING_H
#include "bta_types.h"
#ifndef BTA_V8_PACE_CATCHUP
#define BTA_V8_PACE_CATCHUP 0
#endif
#if BTA_V8_PACE_CATCHUP != 0 && BTA_V8_PACE_CATCHUP != 1
#error BTA_V8_PACE_CATCHUP must be 0 or 1
#endif
/* Clock-paced audio sending with a bounded capture-ring backlog.
 *
 * The 2.7.5 sender sent a packet whenever at least one was waiting and only
 * resynchronised once ~128 ms had piled up, so a scheduling hiccup could leave
 * that much stale audio in flight and the console's own buffer grew to match.
 * This module decides, from the wall clock and the bytes waiting in the capture
 * ring, whether to send now, and how many of the oldest bytes to drop first:
 *
 *   - At the start of playback (and after an underrun) it prebuffers: it holds
 *     off sending until the backlog has built up to the target. That cushion is
 *     the jitter buffer; without it any scheduling jitter would starve the sink.
 *   - Then one packet per interval (clock pacing). The driver is fed at the
 *     playout rate, so the console-side buffer is fed no faster than it drains
 *     and cannot grow, and the cushion is held at the target.
 *   - A backlog above target + one packet is trimmed by dropping the oldest
 *     bytes, so it stays shallow continuously instead of in one 128 ms jump.
 *   - A hard ceiling, well under the ring, catches a long stall: the excess is
 *     dropped at once and the clock realigns.
 *
 * Pure integer logic with no clock, ring layout or float, so a host test drives
 * it with a fake clock and a simulated backlog (tests/test_pacing.c). The worker
 * supplies `now` and `available` and applies `skip` with bta_ring_advance. */

/* Capture-ring bytes turned into one packet, and the packet's playout time. One
 * packet is 192 stereo frames at 48 kHz: 384 floats in, 384 shorts out. */
#define BTA_PACKET_BYTES 1536u
#define BTA_PACKET_US    4000u

/* Latency modes: how much backlog to hold, trading dropout resistance for delay.
 * Target backlog is 8 / 12 / 16 ms; the steady-state ceiling is one packet more. */
enum { BTA_LAT_LOW = 0, BTA_LAT_BALANCED = 1, BTA_LAT_STABLE = 2 };

/* What the worker should do with this slot. RESYNC is a SEND that also dropped a
 * large backlog (the ceiling was hit); UNDERRUN is a WAIT with no whole packet
 * ready. Both distinctions exist only so the worker can count them. */
enum { BTA_PACE_WAIT = 0, BTA_PACE_SEND = 1, BTA_PACE_RESYNC = 2, BTA_PACE_UNDERRUN = 3 };

struct bta_pace { u64 due, last_catchup_at; u32 target, cap, interval, extra_margin; int filling; };
struct bta_pace_step { int kind; u32 skip; int catchup; }; /* skip: ring bytes to drop first */

static u32 bta_pace_target(int mode) {
    u32 packets = mode == BTA_LAT_LOW ? 2u : mode == BTA_LAT_STABLE ? 4u : 3u;
    return packets * BTA_PACKET_BYTES;
}
/* Set the backlog target from the latency mode. The clock and the filling flag
 * are left alone; when the mode is raised mid-stream the worker re-enters
 * prebuffering (apply_latency) so the deeper cushion is actually built. */
static void bta_pace_config(struct bta_pace *p, int mode) {
    p->target = bta_pace_target(mode);
    p->cap = p->target + 4u * BTA_PACKET_BYTES; /* hard-drop ceiling, well under the ring */
    p->interval = BTA_PACKET_US;
    p->extra_margin = 0;
}
/* Session-only tuning: cap stays beyond the permitted steady backlog.
 * 8..32ms cushion and 0..16ms extra margin stay safely within the ring. */
static void bta_pace_tune(struct bta_pace *p, u32 cushion_ms, u32 margin_ms) {
    p->target = (cushion_ms / 4u) * BTA_PACKET_BYTES;
    p->extra_margin = (margin_ms / 4u) * BTA_PACKET_BYTES;
    p->cap = p->target + p->extra_margin + 4u * BTA_PACKET_BYTES;
}
/* Begin a fresh prebuffer at stream start: no sending until the cushion fills. */
static void bta_pace_reset(struct bta_pace *p, u64 now) {
    p->due = now; p->filling = 1; p->last_catchup_at = 0;
}

static struct bta_pace_step bta_pace_run(struct bta_pace *p, u64 now, u32 available) {
    struct bta_pace_step r = {BTA_PACE_WAIT, 0, 0};
    u32 keep = p->target + BTA_PACKET_BYTES + p->extra_margin;
    int hard = available >= p->cap;
    int was_filling = p->filling;
    /* Prebuffer: after a (re)start or an underrun, hold off until the target
     * cushion has built, so jitter has something to draw down. A backlog already
     * past the ceiling stops filling and goes straight to trimming. */
    if (p->filling) {
        if (!hard && available < p->target) return r;
        p->filling = 0;
        p->due = now;
    }
    /* Clock gate: at most one packet per interval. A backlog past the ceiling
     * overrides it so a long stall cannot sit above the ceiling. */
    if (now < p->due && !hard) return r;
    /* Underrun: the cushion emptied. Rebuild it before sending again. */
    if (available < BTA_PACKET_BYTES) { r.kind = BTA_PACE_UNDERRUN; p->filling = 1; p->due = now; return r; }
    r.kind = hard ? BTA_PACE_RESYNC : BTA_PACE_SEND;
#if BTA_V8_PACE_CATCHUP
    /* Opt-in: if one packet deadline was missed and backlog permits, retain
     * one extra packet for a closely-spaced follow-up. This is limited to
     * one catch-up attempt every 20 ms. Never override the hard resync. */
    if (!hard && !was_filling && now > p->due + 6000UL &&
        available >= p->target + 2u * BTA_PACKET_BYTES &&
        (!p->last_catchup_at || now >= p->last_catchup_at + 20000UL)) {
        r.catchup = 1;
        p->last_catchup_at = now;
        keep += BTA_PACKET_BYTES;
    }
#else
    (void)was_filling;
#endif
    if (available > keep) r.skip = available - keep; /* drop oldest excess */
    /* Otherwise preserve the original V7 pacing rule. */
    p->due = r.catchup ? now + 1000UL :
        (hard || now > p->due + p->interval) ? now + p->interval : p->due + p->interval;
    return r;
}
#endif
