#ifndef BTA_FW14_SLOTS_H
#define BTA_FW14_SLOTS_H
/* FW14-only guarded HCI Change Connection Packet Type experiment.
 * No direct kernel-link-state writes. Recover only from observed regression. */
#include "kernel.h"
#define BTA_FW14_SLOTS_MAC 0x78152d1d427cUL /* FreePods 4; V5 behavior */
#define BTA_FW14_SPACESBUDS_MAC 0x2852e035e1ccUL /* SpaceBuds Air, observed only by default */
#define BTA_FW14_SLOTS_MASK 0x3c18u
/* Build-time safety switch. A zero-valued build ONLY observes SpaceBuds and
 * NEVER issues packet-type changes to them. FreePods V5 recovery is unchanged.
 * 1 is an explicitly experimental opt-in build for precisely this SpaceBuds MAC. */
#ifndef BTA_V7_SPACEBUDS_RECOVER
#define BTA_V7_SPACEBUDS_RECOVER 0
#endif
#if BTA_V7_SPACEBUDS_RECOVER != 0 && BTA_V7_SPACEBUDS_RECOVER != 1
#error BTA_V7_SPACEBUDS_RECOVER must be 0 or 1
#endif
static inline int bta_slots_observe_allowed(u64 mac) {
    return mac == BTA_FW14_SLOTS_MAC || mac == BTA_FW14_SPACESBUDS_MAC;
}
static inline int bta_slots_command_target(u64 mac) {
    return mac == BTA_FW14_SLOTS_MAC ||
           (BTA_V7_SPACEBUDS_RECOVER && mac == BTA_FW14_SPACESBUDS_MAC);
}

enum bta_slots_status {
    BTA_SLOTS_OBSERVE = 0, BTA_SLOTS_WAIT_SERVICE = 1,
    BTA_SLOTS_ALREADY_WIDE = 2, BTA_SLOTS_SENT = 3,
    BTA_SLOTS_COMMAND_ERROR = 4, BTA_SLOTS_NO_LINK = 5,
    BTA_SLOTS_UNSUPPORTED = 6, BTA_SLOTS_BAD_TARGET = 7,
    BTA_SLOTS_WAIT_ACTIVE = 8
};
struct bta_slots_sample {
    int status, command_rc;
    u64 link_address;
    u32 channel, mode, maxslots, pktmask, pending, active;
    u32 countdown, pcm, submitted, handle;
};
static inline int bta_slots_dispatch_allowed(u64 mac, unsigned mode,
        unsigned service, unsigned channel, unsigned slots,
        unsigned handle) {
    return bta_slots_command_target(mac) && mode == 0 &&
           (service & 8u) != 0 && channel == 10u &&
           slots == 1u && handle != 0;
}
static inline int bta_slots_wait_for_opportunity(int status) {
    return status == BTA_SLOTS_WAIT_SERVICE || status == BTA_SLOTS_NO_LINK ||
           status == BTA_SLOTS_WAIT_ACTIVE;
}
/* FW14 V5 policy: cap requests per actual Bluetooth connection.
 * For the SAME link, require 20s and confirmed-wide rearm between requests.
 * For a NEW link after a real teardown, keep a 5s cross-link floor so an early
 * Active-mode opportunity is not lost to the original 20s cooldown. */
#define BTA_SLOTS_SESSION_CAP 3u
#define BTA_SLOTS_RECOVERY_COOLDOWN_US 20000000UL
#define BTA_SLOTS_NEW_LINK_COOLDOWN_US 5000000UL
struct bta_slots_recovery_budget {
    unsigned requests;      /* reset only on an actual physical link lifecycle */
    int ever_issued;         /* never reset: protects cooldown across reconnects */
    u64 last_request_at;     /* never reset by a new connection */
};
static inline void bta_slots_recovery_reset_link(
        struct bta_slots_recovery_budget *b) {
    if (b) b->requests = 0;
}
static inline void bta_slots_recovery_record_request(
        struct bta_slots_recovery_budget *b, u64 now) {
    if (!b) return;
    if (b->requests < BTA_SLOTS_SESSION_CAP) b->requests++;
    b->ever_issued = 1;
    b->last_request_at = now;
}
static inline int bta_slots_recovery_narrow(const struct bta_slots_sample *s) {
    return s && s->status == BTA_SLOTS_OBSERVE && s->channel == 10u &&
           (s->active & 8u) != 0 && s->maxslots == 1u && s->handle != 0;
}
static inline int bta_slots_recovery_ready(const struct bta_slots_sample *s,
        const struct bta_slots_recovery_budget *b, int rearmed,
        unsigned narrow_samples, u64 now) {
    return b && b->requests < BTA_SLOTS_SESSION_CAP && narrow_samples >= 2 &&
           (b->requests == 0 || rearmed) &&
           (!b->ever_issued || (now >= b->last_request_at &&
                now - b->last_request_at >=
                  (b->requests == 0 ? BTA_SLOTS_NEW_LINK_COOLDOWN_US :
                                     BTA_SLOTS_RECOVERY_COOLDOWN_US))) &&
           bta_slots_recovery_narrow(s) && s->mode == 0u;
}
int bt_fw14_slots_experiment(u64 mac, int issue, struct bta_slots_sample *sample);
#endif
