#include "test.h"
#include "v12_tone.h"
void test_v12_tone(void) {
    struct bta_v12_tone t;
    bta_v12_tone_reset(&t);
    CHECK_EQ(t.packets, 0);
    CHECK(bta_v12_tone_ready(&t,1000));
    CHECK_EQ(t.due,5000);
    CHECK(!bta_v12_tone_ready(&t,4999));
    CHECK(bta_v12_tone_ready(&t,5000));
    CHECK_EQ(t.due,9000);
    CHECK(bta_v12_tone_ready(&t,9750));
    CHECK_EQ(t.due,13000); /* not now+4000=13750: no cumulative drift */
    CHECK_EQ(t.skipped,0);
    CHECK_EQ(t.packets,3);
    bta_v12_tone_note_result(&t,9750,14000,0);
    CHECK_EQ(t.refused,1); CHECK_EQ(t.ioctl4,1); CHECK_EQ(t.ioctl8,0);
    bta_v12_tone_note_result(&t,9750,19000,-1);
    CHECK_EQ(t.failed,1); CHECK_EQ(t.ioctl8,1); CHECK_EQ(t.maxcall_us,9250);
    bta_v12_tone_reset(&t);
    CHECK(bta_v12_tone_ready(&t,1000));
    CHECK(bta_v12_tone_ready(&t,18000)); /* 13ms late: skip 3 slots */
    CHECK_EQ(t.skipped,3); CHECK_EQ(t.late4,1); CHECK_EQ(t.late8,1);
    CHECK_EQ(t.maxlate_us,13000); CHECK_EQ(t.gaps8,1); CHECK_EQ(t.maxgap_us,17000);
    CHECK_EQ(t.due,21000);
    CHECK(!bta_v12_tone_ready(&t,18001)); /* never burst immediately */
    CHECK(bta_v12_tone_ready(&t,21250));
    CHECK_EQ(t.due,25000); /* rejoin anchored original 4ms clock */

    /* Simulate a 2s test run polled every 750us. V11's now+4ms pacing
     * undersends by 10% at this period, even if every driver call succeeds. */
    for (unsigned poll=250;poll<=1000;poll+=250) {
        bta_v12_tone_reset(&t);
        u64 old_due=0;
        unsigned old_count=0,new_count=0;
        for(u64 now=1000;now<2001000;now+=poll) {
            if(bta_v12_tone_ready(&t,now)) new_count++;
            if(now>=old_due) {old_count++;old_due=now+4000;}
        }
        CHECK_EQ(new_count,500);
        CHECK_EQ(t.packets,new_count);
        CHECK_EQ(t.skipped,0);
        if (poll==750) CHECK(old_count<460); /* V11: ~445 vs V12: 500 */
    }
    /* Stress jitter in polling (300us - 1199us) with a deterministic LCG.
     * Anchor must remain close to 250pps and never oversend. */
    bta_v12_tone_reset(&t);
    u32 rng=12345;
    unsigned emitted=0;
    for(u64 now=1000;now<2001000;) {
        if(bta_v12_tone_ready(&t,now)) emitted++;
        rng = rng*1664525U+1013904223U;
        now += 300u+(rng%900u);
    }
    CHECK(emitted>=499 && emitted<=500);
    CHECK_EQ(emitted,t.packets);
    CHECK_EQ(t.skipped,0);
    bta_v12_tone_reset(&t);
    CHECK_EQ(t.ioctl4,0); CHECK_EQ(t.skipped,0);
}
