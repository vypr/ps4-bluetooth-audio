#include "test.h"
#include "v13_sender.h"
void test_v13_sender(void) {
    struct bta_v13_sender d = {0};
    bta_v13_note(&d, 10000, 10025, 0, 0);
    CHECK(d.loops == 1);
    CHECK(d.maxlock_us == 25);
    CHECK(d.sleep4 == 0);
    bta_v13_note(&d, 14000, 19500, 13000, 750);
    CHECK(d.lock1 == 1);
    CHECK(d.lock4 == 1);
    CHECK(d.lock8 == 0);
    CHECK(d.maxlock_us == 5500);
    CHECK(d.sleep4 == 0);
    CHECK(d.maxsleep_us == 250);
    bta_v13_note(&d, 23100, 32200, 14000, 750);
    CHECK(d.lock8 == 1);
    CHECK(d.sleep8 == 1);
    CHECK(d.maxsleep_us == 8350);
    bta_v13_note(&d, 50000, 50001, 40000, 750);
    CHECK(d.sleep4 == 2);
    CHECK(d.sleep8 == 2);
    bta_v13_reset(&d);
    CHECK(d.loops == 0);
    CHECK(d.maxlock_us == 0);
}
