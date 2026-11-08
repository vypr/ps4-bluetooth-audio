#include "test.h"
#include "v14_hdmi.h"
void test_v14_hdmi(void) {
    CHECK(!bta_v14_mute_needed(0, 1, 8));
    CHECK(!bta_v14_mute_needed(1, 0, 8));
    CHECK(!bta_v14_mute_needed(1, -1, 8));
    CHECK(!bta_v14_mute_needed(1, 1, -1));
    CHECK(bta_v14_mute_needed(1, 1, 8));
    struct bta_v14_hdmi d = {0};
    bta_v14_hdmi_note(&d, 1000, 1200, 0);
    CHECK(d.calls == 1 && d.slow4 == 0 && d.errors == 0);
    bta_v14_hdmi_note(&d, 1000, 7000, 0);
    CHECK(d.slow4 == 1 && d.slow8 == 0);
    bta_v14_hdmi_note(&d, 1000, 11000, -5);
    CHECK(d.calls == 3 && d.slow8 == 1 && d.maxcall_us == 10000);
    CHECK(d.errors == 1);
    bta_v14_hdmi_reset(&d);
    CHECK(d.calls == 0 && d.maxcall_us == 0 && d.errors == 0);
}
