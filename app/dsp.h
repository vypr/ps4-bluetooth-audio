#ifndef BTA_DSP_H
#define BTA_DSP_H
#include "bta_types.h"
/* Pure audio sample maths: everything the sender does to turn the system audio
 * mix into the 16-bit stereo PCM packets the Bluetooth driver takes, with no
 * driver or clock calls. Kept separate so it can be unit-tested on a PC with a
 * plain byte buffer standing in for the capture ring.
 *
 * Uses floating point, so (unlike the firmware adapter) this header must only
 * be included by sources built without -mgeneral-regs-only. */

/* One cycle of a sine at ~8000/32767 (~24%) full scale for the safe test tone. */
static const short BTA_SINE32[32] = {
    0, 1561, 3061, 4445, 5657, 6652, 7391, 7846, 8000, 7846, 7391, 6652, 5657, 4445, 3061, 1561,
    0, -1561, -3061, -4445, -5657, -6652, -7391, -7846, -8000, -7846, -7391, -6652, -5657, -4445, -3061, -1561};

/* Bytes of captured audio waiting between the driver's write cursor and our
 * read cursor, accounting for the ring wrap at 0xfff0. */
static unsigned bta_ring_available(u32 writepos, u32 readpos) {
    return writepos >= readpos ? writepos - readpos : 0xfff0 - readpos + writepos;
}

/* Advance a read cursor by `n` bytes, wrapping the [16, 0x10000) data region the
 * same way bta_convert_capture does (0x10000 -> 16). Used to drop the oldest
 * backlog bytes when the pacer trims. `n` must be a multiple of 8 so the cursor
 * stays stereo-frame aligned. */
static u32 bta_ring_advance(u32 cursor, u32 n) {
    cursor += n;
    while (cursor >= 0x10000) cursor -= 0xfff0;
    return cursor;
}

/* Convert 384 float samples from the capture ring (starting at `cursor`, which
 * wraps to 16 at 0x10000) into 384 shorts, scaled by volume (0..100) and
 * clamped. NaNs become silence. *level and *peak track the running peak
 * magnitude. Returns the advanced cursor. */
static u32 bta_convert_capture(const unsigned char *ring, u32 cursor, int volume,
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

/* Fill one 192-frame stereo test packet (384 shorts) from the sine table,
 * advancing *phase. Follows the volume slider: 50% is the old fixed level,
 * 100% about half of full scale. *level and *peak track the running peak. */
static void bta_test_tone(short pcm[384], u32 *phase, int volume, unsigned *level, unsigned *peak) {
    for (unsigned i = 0; i < 192; i++) {
        short s = (short)(BTA_SINE32[(*phase >> 8) & 31] * 2 * volume / 100);
        *phase += 75;
        pcm[2 * i] = s;
        pcm[2 * i + 1] = s;
        unsigned m = s < 0 ? -s : s;
        if (m > *level) *level = m;
        if (m > *peak) *peak = m;
    }
}
#endif
