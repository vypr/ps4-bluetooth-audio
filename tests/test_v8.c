#include "test.h"
#include "v8_gain.h"
#include "v8_diag.h"
#include "pacing.h"

void test_v8(void) {
    struct bta_v8_gain g;
    bta_v8_gain_reset(&g);
    CHECK_EQ(g.current_permille, BTA_V8_FREEPODS_GAIN_PCT * 10);
    short pcm[384];
    unsigned level, peak;
    for (unsigned i = 0; i < 384; i++) pcm[i] = (short)(i & 1u ? -10000 : 10000);
    level = peak = 0;
    CHECK_EQ(bta_v8_apply_gain(&g, pcm, &level, &peak), 0);
    CHECK_EQ(pcm[0], 10000 * BTA_V8_FREEPODS_GAIN_PCT / 100);
    CHECK_EQ(pcm[1], -10000 * BTA_V8_FREEPODS_GAIN_PCT / 100);
    CHECK_EQ(peak, 10000u * BTA_V8_FREEPODS_GAIN_PCT / 100u);
    for (unsigned i = 0; i < 384; i++) pcm[i] = (short)(i & 1u ? -32768 : 32767);
    level = peak = 0;
    CHECK_EQ(bta_v8_apply_gain(&g, pcm, &level, &peak), 1);
    CHECK(g.current_permille < BTA_V8_FREEPODS_GAIN_PCT * 10);
    CHECK(peak <= 32000);
    for (unsigned i = 0; i < 384; i++) CHECK(pcm[i] <= 32000 && pcm[i] >= -32000);
    int attack_gain = g.current_permille;
    for (unsigned i = 0; i < 384; i++) pcm[i] = (short)(i & 1u ? -100 : 100);
    level = peak = 0;
    bta_v8_apply_gain(&g, pcm, &level, &peak);
    CHECK_EQ(g.current_permille, attack_gain + 10);
    CHECK_EQ(peak, (unsigned)(100 * g.current_permille / 1000));
    struct bta_v8_diag d = {0};
    bta_v8_note_pacer(&d, BTA_PACE_SEND, 1536, 0, 17000, 8000, 1);
    CHECK_EQ(d.trim_bytes, 1536);
    CHECK_EQ(d.trim_events, 1);
    CHECK_EQ(d.max_trim_bytes, 1536);
    CHECK_EQ(d.late4, 1);
    CHECK_EQ(d.late8, 1);
    CHECK_EQ(d.late16, 0);
    CHECK_EQ(d.maxlate_us, 9000);
    CHECK_EQ(d.catchups, 1);
    bta_v8_note_call(&d, 20000, 29001);
    CHECK_EQ(d.calls4, 1);
    CHECK_EQ(d.calls8, 1);
    CHECK_EQ(d.maxcall_us, 9001);
    bta_v8_diag_reset(&d);
    CHECK_EQ(d.trim_events, 0);
    CHECK_EQ(d.calls8, 0);
    /* Force a late packet with enough backlog: only the experimental build
     * should schedule an extra send at +1ms, at most once per 20ms. */
    struct bta_pace p;
    bta_pace_config(&p, BTA_LAT_STABLE);
    bta_pace_reset(&p, 100000);
    p.filling = 0;
    p.due = 100000;
    struct bta_pace_step s = bta_pace_run(&p, 107000, p.target + 2u * BTA_PACKET_BYTES);
#if BTA_V8_PACE_CATCHUP
    CHECK_EQ(s.catchup, 1);
    CHECK_EQ(s.skip, 0);
    CHECK_EQ(p.due, 108000);
#else
    CHECK_EQ(s.catchup, 0);
    CHECK_EQ(s.skip, BTA_PACKET_BYTES);
    CHECK_EQ(p.due, 111000);
#endif
}
