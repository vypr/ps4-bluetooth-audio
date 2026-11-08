#include "test.h"
#include "v9_tune.h"
#include "pacing.h"
void test_v9_tune(void) {
    struct bta_v9_tune t;
    bta_v9_tune_init(&t, 16);
    CHECK_EQ(t.cushion_ms, 16); CHECK_EQ(t.margin_ms, 0); CHECK_EQ(t.poll_us, 500);
    for (unsigned n=8; n<=32; n+=4) {
        CHECK(bta_v9_tune_set(&t, BTA_V9_CUSHION, n)); CHECK_EQ(t.cushion_ms, n);
    }
    for (unsigned n=0; n<=16; n+=4) {
        CHECK(bta_v9_tune_set(&t, BTA_V9_MARGIN, n)); CHECK_EQ(t.margin_ms, n);
    }
    for (unsigned n=250; n<=1000; n+=250) {
        CHECK(bta_v9_tune_set(&t, BTA_V9_POLL, n)); CHECK_EQ(t.poll_us, n);
    }
    for (unsigned n=0;n<100;n++) {
        struct bta_v9_tune saved=t;
        int valid_cushion = n>=8 && n<=32 && n%4==0;
        CHECK_EQ(bta_v9_tune_set(&t, BTA_V9_CUSHION,n), valid_cushion);
        if (valid_cushion) CHECK_EQ(t.cushion_ms,n);
        else CHECK_EQ(t.cushion_ms,saved.cushion_ms);
        saved=t;
        int valid_margin = n<=16 && n%4==0;
        CHECK_EQ(bta_v9_tune_set(&t, BTA_V9_MARGIN,n), valid_margin);
        if (valid_margin) CHECK_EQ(t.margin_ms,n);
        else CHECK_EQ(t.margin_ms,saved.margin_ms);
    }
    CHECK(!bta_v9_tune_set(&t, BTA_V9_POLL, 249));
    CHECK(!bta_v9_tune_set(&t, BTA_V9_POLL, 999));
    CHECK(!bta_v9_tune_set(&t, BTA_V9_POLL, 1250));
    CHECK(!bta_v9_tune_set(&t, 50, 500));
    struct bta_pace pace = {0};
    bta_pace_config(&pace, BTA_LAT_STABLE);
    CHECK_EQ(pace.extra_margin,0);
    bta_pace_tune(&pace, 32, 16);
    CHECK_EQ(pace.target, 8*BTA_PACKET_BYTES);
    CHECK_EQ(pace.extra_margin, 4*BTA_PACKET_BYTES);
    CHECK_EQ(pace.cap, 16*BTA_PACKET_BYTES);
    bta_pace_tune(&pace, 8,0);
    CHECK_EQ(pace.target,2*BTA_PACKET_BYTES);
    CHECK_EQ(pace.cap,6*BTA_PACKET_BYTES);
}
