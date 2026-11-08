#ifndef BTA_V11_VOLUME_H
#define BTA_V11_VOLUME_H
#include "v8_gain.h"
/* The legacy 0..100% volume range is preserved bit-for-bit: capture and test
 * PCM receive the same volume as before. 101..200% holds the base PCM at 100%
 * and adds uniform, peak-limited gain after generation. A FreePods-specific
 * 130% profile multiplier remains present for ALL slider values.
 *
 * These controls adjust PCM amplitude, NOT headset hardware gain. Saturated
 * source audio cannot be made proportionally louder without compression.
 */
#define BTA_V11_MAX_VOLUME 200
static int bta_v11_base_volume(int ui_volume) {
    return ui_volume > 100 ? 100 : ui_volume;
}
static int bta_v11_gain_pct(int ui_volume, int freepods) {
    const int post = ui_volume > 100 ? ui_volume : 100;
    return post * (freepods ? BTA_V8_FREEPODS_GAIN_PCT : 100) / 100;
}
static int bta_v11_apply_volume(struct bta_v8_gain *gain, short pcm[384],
                                unsigned *level, unsigned *peak, int volume,
                                int freepods) {
    int pct = bta_v11_gain_pct(volume, freepods);
    /* No effect on ordinary SpaceBuds audio at 0..100%, just as V10. */
    if (pct == 100) return 0;
    return bta_v8_apply_gain_pct(gain, pcm, level, peak, pct);
}
#endif
