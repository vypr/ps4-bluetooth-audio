#include "test.h"
#include "diag_v6.h"
void test_diag_v6(void) {
    struct bta_diag6_window d = {0};
    bta_diag6_note(&d, 0, 1, 3600, 100000, 101000);
    CHECK_EQ(d.game_packets, 1);
    CHECK_EQ(d.tone_packets, 0);
    CHECK_EQ(d.game_nonzero, 1);
    CHECK_EQ(d.game_peak, 3600);
    CHECK_EQ(d.rc_one, 1);
    CHECK_EQ(d.max_send_us, 1000);
    CHECK_EQ(d.max_gap_us, 0);
    bta_diag6_note(&d, 1, 768, 5000, 104000, 113000);
    CHECK_EQ(d.tone_packets, 1);
    CHECK_EQ(d.tone_nonzero, 1);
    CHECK_EQ(d.tone_peak, 5000);
    CHECK_EQ(d.rc_bytes, 1);
    CHECK_EQ(d.max_gap_us, 4000);
    CHECK_EQ(d.max_send_us, 9000);
    bta_diag6_note(&d, 0, 0, 0, 120000, 120100);
    CHECK_EQ(d.game_packets, 2);
    CHECK_EQ(d.game_nonzero, 1);
    CHECK_EQ(d.refused, 1);
    CHECK_EQ(d.max_gap_us, 16000);
    bta_diag6_note(&d, 1, -3, 0, 120001, 120010);
    bta_diag6_note(&d, 0, 99, 1, 120002, 120020);
    CHECK_EQ(d.failed, 1);
    CHECK_EQ(d.rc_other, 1);
    CHECK_EQ(d.last_rc, 99);
    CHECK_EQ(d.game_peak, 3600);
    CHECK_EQ(d.tone_peak, 5000);
    bta_diag6_reset(&d);
    CHECK_EQ(d.game_packets, 0);
    CHECK_EQ(d.tone_packets, 0);
    CHECK_EQ(d.rc_bytes, 0);
    CHECK_EQ(d.last_rc, 0);
    CHECK_EQ(d.last_send_at, 0);
    /* Saturate times instead of truncating signed/unsigned timestamp deltas. */
    bta_diag6_note(&d, 0, 768, 0, 100000, (u64)0xffffffffUL + 100001);
    CHECK_EQ(d.max_send_us, 0xffffffffUL);
    bta_diag6_note(&d, 0, 768, 0, 1, 1); /* clock rollback cannot underflow */
    CHECK_EQ(d.max_send_us, 0xffffffffUL);
    CHECK_EQ(d.max_gap_us, 0);
}
