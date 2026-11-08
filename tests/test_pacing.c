#include "test.h"
#include "pacing.h"

/* Pacing is pure integer logic, so these tests drive it with a fake clock and a
 * plain byte counter standing in for the capture-ring backlog. The final test is
 * a producer/consumer simulation that starts from an empty ring exactly as the
 * worker does, fills at the real 48 kHz rate and ticks every 0.5 ms like the
 * console sender. It asserts the cushion is built once, that steady play never
 * underruns, that the audio handed to the driver is never more than the target
 * plus one packet old (the "under ~20 ms" guarantee), and that a long stall is
 * absorbed by a single resync. */

#define RATE_BYTES_PER_TICK 192u /* 48 kHz stereo float over a 500 us sender tick */
#define TICK_US 500u

void test_pacing(void) {
    /* Targets: 8 / 12 / 16 ms (2 / 3 / 4 packets); ceiling is target + 4 packets. */
    CHECK_EQ(bta_pace_target(BTA_LAT_LOW), 2u * BTA_PACKET_BYTES);
    CHECK_EQ(bta_pace_target(BTA_LAT_BALANCED), 3u * BTA_PACKET_BYTES);
    CHECK_EQ(bta_pace_target(BTA_LAT_STABLE), 4u * BTA_PACKET_BYTES);
    CHECK_EQ(bta_pace_target(99), 3u * BTA_PACKET_BYTES); /* unknown -> balanced */

    struct bta_pace p;
    bta_pace_config(&p, BTA_LAT_BALANCED);
    CHECK_EQ(p.target, 3u * BTA_PACKET_BYTES);
    CHECK_EQ(p.cap, 3u * BTA_PACKET_BYTES + 4u * BTA_PACKET_BYTES);
    CHECK_EQ(p.interval, BTA_PACKET_US);
    u32 keep = p.target + BTA_PACKET_BYTES;

    /* Prebuffer: a fresh pacer holds off sending (not an underrun) until the
     * backlog reaches the target, then sends. */
    bta_pace_reset(&p, 0);
    CHECK(p.filling);
    {
        struct bta_pace_step s = bta_pace_run(&p, 0, p.target - BTA_PACKET_BYTES);
        CHECK_EQ(s.kind, BTA_PACE_WAIT); /* still filling */
        CHECK(p.filling);
        s = bta_pace_run(&p, 100, p.target);
        CHECK_EQ(s.kind, BTA_PACE_SEND); /* cushion ready */
        CHECK(!p.filling);
        CHECK_EQ(p.due, 100 + BTA_PACKET_US);
    }

    /* Clock gate: past the prebuffer, with one packet over target waiting, one
     * SEND per interval and nothing in between. */
    bta_pace_reset(&p, 0); p.filling = 0; p.due = 0; /* already past prebuffer */
    {
        int sends = 0;
        for (u64 t = 0; t < 100000; t += TICK_US) {
            struct bta_pace_step s = bta_pace_run(&p, t, keep);
            if (s.kind == BTA_PACE_SEND) { sends++; CHECK_EQ(s.skip, 0); }
            else CHECK_EQ(s.kind, BTA_PACE_WAIT);
        }
        CHECK_EQ(sends, 100000 / (int)BTA_PACKET_US); /* 25, i.e. the playout rate */
    }

    /* Underrun: once streaming, no whole packet -> UNDERRUN, and the pacer drops
     * back into prebuffer so the cushion is rebuilt. */
    bta_pace_reset(&p, 10000); p.filling = 0; p.due = 10000;
    {
        struct bta_pace_step s = bta_pace_run(&p, 10000, BTA_PACKET_BYTES - 8);
        CHECK_EQ(s.kind, BTA_PACE_UNDERRUN);
        CHECK_EQ(s.skip, 0);
        CHECK(p.filling); /* rebuild the cushion before resuming */
    }

    /* Trim: a backlog above target + one packet (but under the ceiling) is a
     * SEND that drops the oldest excess back toward the target. */
    bta_pace_reset(&p, 0); p.filling = 0; p.due = 0;
    {
        u32 available = p.target + 2u * BTA_PACKET_BYTES; /* keep + one packet */
        struct bta_pace_step s = bta_pace_run(&p, 0, available);
        CHECK_EQ(s.kind, BTA_PACE_SEND);
        CHECK_EQ(s.skip, BTA_PACKET_BYTES);        /* trims one packet */
        CHECK_EQ(available - s.skip, keep);        /* leaves target + the packet read */
    }

    /* Ceiling overrides the clock: not yet due, but a backlog at the ceiling
     * forces a RESYNC that drops to the target and realigns the clock. */
    bta_pace_reset(&p, 1000000); p.filling = 0; p.due = 1000000;
    {
        u32 available = p.cap + 5000; /* well past the ceiling */
        struct bta_pace_step s = bta_pace_run(&p, 1000, available); /* now < due */
        CHECK_EQ(s.kind, BTA_PACE_RESYNC);
        CHECK_EQ(available - s.skip, keep);            /* read one packet, leave target */
        CHECK_EQ(p.due, 1000 + BTA_PACKET_US);         /* clock realigned to now */
    }

    /* Producer/consumer simulation from an empty ring, as the worker starts.
     * Invariants:
     *   - the cushion is prebuffered once, then held;
     *   - steady play never underruns;
     *   - every SEND/RESYNC hands over audio at most `keep` bytes old
     *     (service buffer <= target + one packet = 12 / 16 / 20 ms);
     *   - a 60 ms stall is absorbed by exactly one RESYNC, not a silent drift. */
    for (int mode = BTA_LAT_LOW; mode <= BTA_LAT_STABLE; mode++) {
        bta_pace_config(&p, mode);
        bta_pace_reset(&p, 0);
        keep = p.target + BTA_PACKET_BYTES;
        u32 available = 0;        /* empty ring at stream start */
        int sends = 0, resyncs = 0, underruns = 0, started = 0, underruns_after_start = 0;
        u32 worst_age = 0;
        u64 t = 0;
        for (int tick = 0; tick < 2200; tick++, t += TICK_US) {
            /* A 60 ms stall a third of the way in: the producer keeps filling the
             * ring but the sender never runs (console thread starved). */
            int stalled = tick >= 900 && tick < 1020;
            available += RATE_BYTES_PER_TICK;
            if (stalled) continue;
            struct bta_pace_step s = bta_pace_run(&p, t, available);
            if (s.kind == BTA_PACE_WAIT) continue;
            if (s.kind == BTA_PACE_UNDERRUN) { underruns++; if (started) underruns_after_start++; continue; }
            started = 1;
            u32 age = available - s.skip; /* bytes of audio in front of the driver */
            if (age > worst_age) worst_age = age;
            CHECK(age <= keep); /* the core latency guarantee */
            available -= s.skip + BTA_PACKET_BYTES;
            CHECK(available <= keep); /* backlog left shallow after the slot */
            if (s.kind == BTA_PACE_RESYNC) resyncs++; else sends++;
        }
        /* Cushion built: ~1 s of play at 4 ms/packet, minus the 60 ms stall and
         * the prebuffer, is ~255 packets, and no underruns once playing. */
        CHECK(sends >= 230 && sends <= 272);
        CHECK_EQ(underruns_after_start, 0);
        /* The 60 ms stall is caught by a single resync, not a creeping backlog. */
        CHECK_EQ(resyncs, 1);
        /* Worst in-flight audio stayed under 20 ms (keep is 12 / 16 / 20 ms). */
        CHECK(worst_age <= keep);
        CHECK(worst_age * 1000u / 384u <= 20000u); /* bytes -> us, <= 20 ms */
    }

    /* Clock drift: the producer runs ~1% slow against the send clock. The cushion
     * absorbs it, so underruns are rare (a re-prebuffer each time), not the
     * per-tick storm that an uncushioned pacer would produce. */
    {
        bta_pace_config(&p, BTA_LAT_BALANCED);
        bta_pace_reset(&p, 0);
        u32 available = 0;
        int underruns = 0, sends = 0;
        u64 t = 0;
        for (int tick = 0; tick < 4000; tick++, t += TICK_US) {
            available += 190; /* ~1% below the 192 B/tick playout rate */
            struct bta_pace_step s = bta_pace_run(&p, t, available);
            if (s.kind == BTA_PACE_UNDERRUN) { underruns++; continue; }
            if (s.kind != BTA_PACE_SEND && s.kind != BTA_PACE_RESYNC) continue;
            available -= s.skip + BTA_PACKET_BYTES;
            sends++;
        }
        CHECK(sends > 400);   /* kept playing throughout */
        CHECK(underruns <= 5); /* a handful of re-prebuffers, not ~2000 */
    }
}
