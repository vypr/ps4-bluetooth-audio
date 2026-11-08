#include "test.h"
#include "v11_volume.h"
#include "dsp.h"
void test_v11_volume(void) {
    CHECK_EQ(BTA_V11_MAX_VOLUME, 200);
    CHECK_EQ(bta_v11_base_volume(0), 0);
    CHECK_EQ(bta_v11_base_volume(40), 40);
    CHECK_EQ(bta_v11_base_volume(100), 100);
    CHECK_EQ(bta_v11_base_volume(150), 100);
    CHECK_EQ(bta_v11_base_volume(200), 100);
    CHECK_EQ(bta_v11_gain_pct(50, 0), 100);
    CHECK_EQ(bta_v11_gain_pct(100, 0), 100);
    CHECK_EQ(bta_v11_gain_pct(150, 0), 150);
    CHECK_EQ(bta_v11_gain_pct(200, 0), 200);
    CHECK_EQ(bta_v11_gain_pct(85, 1), 130);
    CHECK_EQ(bta_v11_gain_pct(150, 1), 195);
    CHECK_EQ(bta_v11_gain_pct(200, 1), 260);
    short pcm[384];
    unsigned level=0, peak=0;
    struct bta_v8_gain g;
    bta_v8_gain_reset(&g);
    for (unsigned i=0;i<384;i++) pcm[i]=i&1 ? -10000 : 10000;
    CHECK_EQ(bta_v11_apply_volume(&g,pcm,&level,&peak,80,0),0);
    CHECK_EQ(pcm[0],10000); CHECK_EQ(pcm[1],-10000);
    CHECK_EQ(peak,0);
    bta_v8_gain_reset(&g);
    level=peak=0;
    /* The V8 limiter ramps gain upwards by one percent per 4ms packet.
     * Test at its settled gain, not immediately after creating a session. */
    for (unsigned n=0; n<24; n++) {
        for(unsigned i=0;i<384;i++) pcm[i]=i&1 ? -10000:10000;
        level=peak=0;
        CHECK_EQ(bta_v11_apply_volume(&g,pcm,&level,&peak,150,0),0);
    }
    CHECK_EQ(pcm[0],15000); CHECK_EQ(peak,15000);
    bta_v8_gain_reset(&g);
    level=peak=0;
    for(unsigned i=0;i<384;i++) pcm[i]=i&1 ? -10000:10000;
    for (unsigned n=0; n<70; n++) {
        for(unsigned i=0;i<384;i++) pcm[i]=i&1 ? -10000:10000;
        level=peak=0;
        CHECK_EQ(bta_v11_apply_volume(&g,pcm,&level,&peak,150,1),0);
    }
    CHECK_EQ(pcm[0],19500); CHECK_EQ(peak,19500);
    bta_v8_gain_reset(&g);
    for(unsigned i=0;i<384;i++) pcm[i]=i&1 ? -30000:30000;
    level=peak=0;
    CHECK_EQ(bta_v11_apply_volume(&g,pcm,&level,&peak,200,1),1);
    CHECK(peak<=BTA_V8_PCM_CEILING);
    for(unsigned i=0;i<384;i++) CHECK(pcm[i]<=BTA_V8_PCM_CEILING && pcm[i]>=-BTA_V8_PCM_CEILING);
    bta_v8_gain_reset(&g);
    for(unsigned i=0;i<384;i++) pcm[i]=i&1 ? -12000:12000;
    level=peak=0;
    CHECK_EQ(bta_v11_apply_volume(&g,pcm,&level,&peak,85,1),0);
    CHECK_EQ(pcm[0],15600); /* 130% V10 parity */
    /* Tone volume above 100 is implemented as base 100 + post gain. */
    u32 phase=0;
    level=peak=0;
    bta_test_tone(pcm,&phase,bta_v11_base_volume(200),&level,&peak);
    CHECK_EQ(pcm[0],0); CHECK_EQ(pcm[1],0);
    CHECK_EQ(pcm[16],6122); CHECK_EQ(pcm[17],6122);
    bta_v8_gain_reset(&g);
    level=peak=0;
    for (unsigned n=0;n<75;n++) {
        bta_test_tone(pcm,&phase,bta_v11_base_volume(200),&level,&peak);
        bta_v11_apply_volume(&g,pcm,&level,&peak,200,0);
    }
    /* A 200% boost yields a protected near-full-scale 440Hz sine. */
    CHECK(peak>=15000);
    CHECK(peak<=BTA_V8_PCM_CEILING);
}
