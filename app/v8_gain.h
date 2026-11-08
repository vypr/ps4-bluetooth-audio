#ifndef BTA_V8_GAIN_H
#define BTA_V8_GAIN_H
#include "bta_types.h"
/* V8 headset-specific digital boost. FreePods only, other headsets unchanged.
 * A 4-ms PCM packet is inspected before applying a UNIFORM scalar: this avoids
 * per-sample waveshaping. A maximum peak of 32000 prevents sample clipping.
 * Attack is immediate to avoid saturation; recovery is 10 permille per 4-ms
 * packet (~120 ms for 100->130%) to reduce pumping between sounds.
 * This is not a guarantee of +30% acoustic loudness: earbuds have their own gain.
 */
#define BTA_V8_FREEPODS_MAC 0x78152d1d427cUL
#ifndef BTA_V8_FREEPODS_GAIN_PCT
#define BTA_V8_FREEPODS_GAIN_PCT 130
#endif
#if BTA_V8_FREEPODS_GAIN_PCT < 100 || BTA_V8_FREEPODS_GAIN_PCT > 140
#error BTA_V8_FREEPODS_GAIN_PCT must be in [100,140]
#endif
#define BTA_V8_PCM_CEILING 32000
struct bta_v8_gain { int current_permille; };
static void bta_v8_gain_reset(struct bta_v8_gain *g) {
    g->current_permille = BTA_V8_FREEPODS_GAIN_PCT * 10;
}
/* Returns 1 when full requested gain was restricted by the limiter. */
static int bta_v8_apply_gain_pct(struct bta_v8_gain *g, short pcm[384],
                                  unsigned *level, unsigned *peak, int requested_pct) {
    /* 100..260 percent in V11 (SpaceBuds <=200, FreePods <=260).
     * The independent V8 wrapper retains the original 130% behavior. */
    if (requested_pct < 100) requested_pct = 100;
    if (requested_pct > 260) requested_pct = 260;
    const int target = requested_pct * 10;
    unsigned raw_peak = 0;
    for (unsigned i = 0; i < 384; i++) {
        int v = pcm[i]; unsigned m = (unsigned)(v < 0 ? -v : v);
        if (m > raw_peak) raw_peak = m;
    }
    int safe = target;
    if (raw_peak && (u64)raw_peak * (u64)target > (u64)BTA_V8_PCM_CEILING * 1000UL)
        safe = (int)((u64)BTA_V8_PCM_CEILING * 1000UL / raw_peak);
    if (g->current_permille <= 0 || g->current_permille > target) g->current_permille = target;
    if (safe < g->current_permille) g->current_permille = safe; /* fast attack */
    else if (safe > g->current_permille) {
        g->current_permille += 10; /* slow release: up to 1% every packet */
        if (g->current_permille > safe) g->current_permille = safe;
    }
    unsigned out_peak = 0;
    for (unsigned i = 0; i < 384; i++) {
        int v = (int)((long)pcm[i] * (long)g->current_permille / 1000L);
        /* Defensive clamp; the peak gate should already keep it below ceiling. */
        if (v > BTA_V8_PCM_CEILING) v = BTA_V8_PCM_CEILING;
        if (v < -BTA_V8_PCM_CEILING) v = -BTA_V8_PCM_CEILING;
        pcm[i] = (short)v;
        unsigned m = (unsigned)(v < 0 ? -v : v);
        if (m > out_peak) out_peak = m;
    }
    if (out_peak > *level) *level = out_peak;
    *peak = out_peak;
    return safe < target;
}
static inline int bta_v8_apply_gain(struct bta_v8_gain *g, short pcm[384],
                              unsigned *level, unsigned *peak) {
    return bta_v8_apply_gain_pct(g, pcm, level, peak, BTA_V8_FREEPODS_GAIN_PCT);
}
#endif
