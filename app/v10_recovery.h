#ifndef BTA_V10_RECOVERY_H
#define BTA_V10_RECOVERY_H
#include "bta_types.h"
/* A manual user action only. Never infer audible playback from driver acceptance.
 * A 20-second inter-request cooldown protects a headset and the PS4 radio from
 * repeated link teardown loops. Units are the monotonic microsecond clock. */
#define BTA_V10_RESET_COOLDOWN_US 20000000UL
#define BTA_V10_RESET_AUDIO 1
#define BTA_V10_RESET_LINK 2
static int bta_v10_reset_allowed(int streaming, u64 address, u64 now, u64 next_allowed) {
    return streaming && address && now >= next_allowed;
}
static unsigned bta_v10_reset_wait_ms(u64 now, u64 next_allowed) {
    if (now >= next_allowed) return 0;
    u64 left = (next_allowed - now + 999UL) / 1000UL;
    return left > 0xffffffffUL ? 0xffffffffu : (unsigned)left;
}
#endif
