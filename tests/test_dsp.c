#include "test.h"
#include "dsp.h"
#include <string.h>
#include <stdlib.h>

/* A quiet NaN without pulling in <math.h>/libm, so the tests build with any
 * native compiler and no extra link flags. */
static float test_nan(void) { unsigned x = 0x7fc00000u; float f; memcpy(&f, &x, 4); return f; }

/* Reference implementations copied verbatim from the pre-refactor worker.c
 * inline audio loops. The point of these tests is to prove the extracted
 * dsp.h helpers compute bit-for-bit what the shipped 2.7.5 service did, over
 * many randomised inputs, not merely a few hand-picked cases. */

static const short REF_SINE32[32] = {
    0, 1561, 3061, 4445, 5657, 6652, 7391, 7846, 8000, 7846, 7391, 6652, 5657, 4445, 3061, 1561,
    0, -1561, -3061, -4445, -5657, -6652, -7391, -7846, -8000, -7846, -7391, -6652, -5657, -4445, -3061, -1561};

static u32 ref_convert(const unsigned char *ring, u32 cursor, int volume,
                       short pcm[384], unsigned *level, unsigned *peak) {
    for (unsigned i = 0; i < 384; i++) {
        float f = *(const volatile float *)(ring + cursor);
        cursor += 4;
        if (cursor == 0x10000) cursor = 16;
        if (f != f) f = 0;
        if (f > 1) f = 1;
        if (f < -1) f = -1;
        int v = (int)(f * (32767.0f * (float)volume / 100.0f));
        pcm[i] = (short)v;
        unsigned m = v < 0 ? -v : v;
        if (m > *level) *level = m;
        if (m > *peak) *peak = m;
    }
    return cursor;
}
static void ref_tone(short pcm[384], u32 *phase, int volume, unsigned *level, unsigned *peak) {
    for (unsigned i = 0; i < 192; i++) {
        short s = (short)(REF_SINE32[(*phase >> 8) & 31] * 2 * volume / 100);
        *phase += 75;
        pcm[2 * i] = s;
        pcm[2 * i + 1] = s;
        unsigned m = s < 0 ? -s : s;
        if (m > *level) *level = m;
        if (m > *peak) *peak = m;
    }
}

static void put_float(unsigned char *ring, u32 off, float v) { memcpy(ring + off, &v, 4); }

void test_dsp(void) {
    /* bta_ring_available: straight case and the wrap case at 0xfff0. */
    CHECK_EQ(bta_ring_available(0x1000, 0x400), 0xc00);
    CHECK_EQ(bta_ring_available(0x400, 0x1000), (long)(0xfff0 - 0x1000 + 0x400));
    CHECK_EQ(bta_ring_available(0x500, 0x500), 0);

    /* bta_ring_advance: straight step, the wrap at 0x10000 -> 16, and that a
     * step landing exactly on 0x10000 wraps to 16 like bta_convert_capture. */
    CHECK_EQ(bta_ring_advance(0x400, 0x100), 0x500);
    CHECK_EQ(bta_ring_advance(0xfffc, 4), 16);                 /* 0x10000 -> 16 */
    CHECK_EQ(bta_ring_advance(0xfffc, 1536), 16 + 1532);       /* 4 bytes then wrap */
    CHECK_EQ(bta_ring_advance(16, 0xfff0), 16);                /* a whole lap */

    static unsigned char ring[0x10000];

    /* A hand-built case: known floats -> known PCM at volume 100 and 50,
     * with clamping and a NaN mapped to silence. */
    memset(ring, 0, sizeof ring);
    put_float(ring, 16, 0.0f);
    put_float(ring, 20, 1.0f);
    put_float(ring, 24, -1.0f);
    put_float(ring, 28, 2.0f);    /* clamps to +1 */
    put_float(ring, 32, -5.0f);   /* clamps to -1 */
    put_float(ring, 36, 0.5f);
    put_float(ring, 40, test_nan()); /* -> 0 */
    {
        short pcm[384];
        unsigned level = 0, peak = 0;
        bta_convert_capture(ring, 16, 100, pcm, &level, &peak);
        CHECK_EQ(pcm[0], 0);
        CHECK_EQ(pcm[1], 32767);
        CHECK_EQ(pcm[2], -32767);
        CHECK_EQ(pcm[3], 32767);
        CHECK_EQ(pcm[4], -32767);
        CHECK_EQ(pcm[5], (short)(0.5f * 32767.0f));
        CHECK_EQ(pcm[6], 0);
        CHECK_EQ(peak, 32767);
        CHECK_EQ(level, 32767);
    }
    /* Volume 0 produces pure silence regardless of input. */
    {
        short pcm[384];
        unsigned level = 0, peak = 0;
        bta_convert_capture(ring, 16, 0, pcm, &level, &peak);
        for (int i = 0; i < 384; i++) CHECK(pcm[i] == 0);
        CHECK_EQ(peak, 0);
    }
    /* Cursor wraps from near the top of the ring back to offset 16. */
    {
        short pcm[384];
        unsigned level = 0, peak = 0;
        u32 end = bta_convert_capture(ring, 0x10000 - 4, 100, pcm, &level, &peak);
        CHECK_EQ(end, 16 + (384 - 1) * 4);
    }

    /* Randomised equivalence against the pre-refactor reference. */
    srand(12345);
    for (int trial = 0; trial < 2000; trial++) {
        for (u32 o = 0; o < sizeof ring; o += 4) {
            int pick = rand() % 10;
            float f;
            if (pick == 0) f = test_nan();
            else if (pick == 1) f = (float)(rand() % 7 - 3);      /* out-of-range, forces clamp */
            else f = (float)(rand() - RAND_MAX / 2) / (float)(RAND_MAX / 2); /* ~[-1,1] */
            put_float(ring, o, f);
        }
        int volume = rand() % 101;
        u32 start = 16 + (u32)(rand() % ((0x10000 - 16 - 384 * 4) / 8)) * 8;

        short pa[384], pb[384];
        unsigned la = 3, lb = 3, ka = 0, kb = 0; /* seed level non-zero to test the max logic */
        u32 ca = ref_convert(ring, start, volume, pa, &la, &ka);
        u32 cb = bta_convert_capture(ring, start, volume, pb, &lb, &kb);
        CHECK(ca == cb);
        CHECK(la == lb && ka == kb);
        if (memcmp(pa, pb, sizeof pa) != 0) { CHECK(0); break; }
    }

    /* Test tone: stereo duplication, exact values, and reference equivalence. */
    {
        short pcm[384];
        u32 phase = 0;
        unsigned level = 0, peak = 0;
        bta_test_tone(pcm, &phase, 50, &level, &peak);
        for (int i = 0; i < 192; i++) CHECK(pcm[2 * i] == pcm[2 * i + 1]);
        CHECK_EQ(phase, 192 * 75);
    }
    for (int trial = 0; trial < 500; trial++) {
        int volume = rand() % 101;
        u32 pa_phase = (u32)rand() * 4u, pb_phase = pa_phase;
        short pa[384], pb[384];
        unsigned la = 0, lb = 0, ka = 0, kb = 0;
        ref_tone(pa, &pa_phase, volume, &la, &ka);
        bta_test_tone(pb, &pb_phase, volume, &lb, &kb);
        CHECK(pa_phase == pb_phase);
        CHECK(la == lb && ka == kb);
        if (memcmp(pa, pb, sizeof pa) != 0) { CHECK(0); break; }
    }
}
