#include "test.h"
#include "v10_recovery.h"
void test_v10_recovery(void) {
    CHECK(!bta_v10_reset_allowed(0, 0x123u, 30000000UL, 0));
    CHECK(!bta_v10_reset_allowed(1, 0, 30000000UL, 0));
    CHECK(bta_v10_reset_allowed(1, 0x123u, 30000000UL, 0));
    CHECK(!bta_v10_reset_allowed(1, 0x123u, 31000000UL, 50000000UL));
    CHECK(bta_v10_reset_allowed(1, 0x123u, 50000000UL, 50000000UL));
    CHECK(bta_v10_reset_wait_ms(31000123UL, 50000000UL) == 19000u);
    CHECK(bta_v10_reset_wait_ms(50000000UL, 50000000UL) == 0u);
    CHECK(bta_v10_reset_wait_ms(90000000UL, 50000000UL) == 0u);
    CHECK(BTA_V10_RESET_AUDIO != BTA_V10_RESET_LINK);
}
