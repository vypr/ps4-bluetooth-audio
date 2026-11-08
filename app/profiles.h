#ifndef BTA_PROFILES_H
#define BTA_PROFILES_H
#include "bta_types.h"
#include "protocol.h"
#include "conn.h"
/* Device profiles: a small saved record per remembered headphone. This replaces
 * the pre-refactor single `device.bin` plus the per-device `dual-<address>` flag
 * files with one versioned table that also drives automatic behaviour:
 *
 *   - multiple remembered devices (connect to whichever one is turned on);
 *   - the last SBC mode that produced sound, so a device that needs dual channel
 *     starts in dual channel next time instead of connecting silently;
 *   - success and failure counts and the last-connected time, used to choose
 *     between remembered devices and to evict the least useful when the table
 *     is full.
 *
 * Everything here is pure: the worker owns the atomic file write and passes the
 * table in and out, so the whole policy is unit-tested on the host. The on-disk
 * format is the struct itself, guarded by a magic and a version. */

#define BTA_PROFILE_MAX 8
#define BTA_PROFILES_MAGIC   0x50415442u /* "BTAP" */
#define BTA_PROFILES_VERSION 1u

struct bta_profile {
    u64 address;
    u32 cod;        /* class of device (for the audio check and pairing tips) */
    u32 services;   /* service bitmask (bit 3 = audio) */
    u8  mode;       /* last working SBC mode: SBC_JOINT or SBC_DUAL */
    u8  pad[7];
    u32 ok_count;   /* streams that produced sound */
    u32 fail_count; /* attempts that failed */
    u64 last_ok_us; /* process time of the last successful stream */
    char name[96];
};
struct bta_profiles {
    u32 magic, version, count, reserved;
    struct bta_profile items[BTA_PROFILE_MAX];
};

static void bta_profiles_init(struct bta_profiles *p) {
    memset(p, 0, sizeof(*p));
    p->magic = BTA_PROFILES_MAGIC;
    p->version = BTA_PROFILES_VERSION;
}
/* A table read from disk is only trusted when its size, magic, version and
 * count all check out; anything else is treated as "no profiles yet". */
static int bta_profiles_valid(const struct bta_profiles *p, u64 size) {
    return size == sizeof(*p) && p->magic == BTA_PROFILES_MAGIC &&
           p->version == BTA_PROFILES_VERSION && p->count <= BTA_PROFILE_MAX;
}
static int bta_profile_find(const struct bta_profiles *p, u64 address) {
    for (unsigned i = 0; i < p->count; i++)
        if (p->items[i].address == address) return (int)i;
    return -1;
}
/* The entry to drop when the table is full: least recently connected, breaking
 * ties on the fewest successes. */
static int bta_profile_evict(const struct bta_profiles *p) {
    int worst = 0;
    for (unsigned i = 1; i < p->count; i++) {
        const struct bta_profile *w = &p->items[worst], *c = &p->items[i];
        if (c->last_ok_us < w->last_ok_us ||
            (c->last_ok_us == w->last_ok_us && c->ok_count < w->ok_count))
            worst = (int)i;
    }
    return worst;
}
/* Remember a device the user selected (or refresh its name/class), without
 * disturbing its mode or counts. Returns the entry's index. */
static int bta_profile_touch(struct bta_profiles *p, const struct bt_device *d) {
    int i = bta_profile_find(p, d->address);
    if (i < 0) {
        i = p->count < BTA_PROFILE_MAX ? (int)p->count++ : bta_profile_evict(p);
        struct bta_profile *e = &p->items[i];
        memset(e, 0, sizeof(*e));
        e->address = d->address;
        e->mode = SBC_JOINT;
    }
    struct bta_profile *e = &p->items[i];
    e->cod = d->cod;
    e->services = d->services;
    unsigned k = 0;
    for (; d->name[k] && k < sizeof(e->name) - 1; k++) e->name[k] = d->name[k];
    e->name[k] = 0;
    return i;
}
/* Record that a device streamed sound in `mode`. */
static void bta_profile_record_ok(struct bta_profiles *p, u64 address, int mode, u64 now) {
    int i = bta_profile_find(p, address);
    if (i < 0) return;
    p->items[i].mode = (u8)(mode == SBC_DUAL ? SBC_DUAL : SBC_JOINT);
    p->items[i].ok_count++;
    p->items[i].last_ok_us = now;
}
static void bta_profile_record_fail(struct bta_profiles *p, u64 address) {
    int i = bta_profile_find(p, address);
    if (i >= 0) p->items[i].fail_count++;
}
/* A device that has played sound through the service before, not just one that
 * was once selected: the home-screen app lists only these (status `known`). */
static int bta_profile_known(const struct bta_profiles *p, u64 address) {
    int i = bta_profile_find(p, address);
    return i >= 0 && (p->items[i].ok_count > 0 || p->items[i].last_ok_us > 0);
}
/* The SBC mode to start a device in: its remembered working mode, or joint. */
static int bta_profile_mode(const struct bta_profiles *p, u64 address) {
    int i = bta_profile_find(p, address);
    return (i >= 0 && p->items[i].mode == SBC_DUAL) ? SBC_DUAL : SBC_JOINT;
}
/* Set a device's mode from a manual user preference, without touching counts. */
static void bta_profile_set_mode(struct bta_profiles *p, u64 address, int mode) {
    int i = bta_profile_find(p, address);
    if (i >= 0) p->items[i].mode = (u8)(mode == SBC_DUAL ? SBC_DUAL : SBC_JOINT);
}
/* Fill a bt_device from a remembered profile (to make it the active target when
 * auto-selecting). */
static void bta_profile_device(const struct bta_profile *e, struct bt_device *d) {
    memset(d, 0, sizeof(*d));
    d->address = e->address;
    d->cod = e->cod;
    d->services = e->services;
    for (unsigned k = 0; k < sizeof(d->name) && k < sizeof(e->name); k++) d->name[k] = e->name[k];
    d->name[sizeof(d->name) - 1] = 0;
}
/* Among the devices currently in range (`scanned`), the remembered device to
 * connect to: the one most recently used, breaking ties on more successes.
 * Returns its address, or 0 when no remembered device is in range. This is how
 * startup "connect to the last one that is turned on" chooses between several.
 *
 * Presence is address-only on purpose. A device is in this table because it
 * already played sound through the service, which outranks anything its class
 * of device claims; and when the driver's device cache is unavailable the list
 * is built from scan events, where every entry has class of device alone and no
 * service bits. Filtering on bt_is_audio here would make a headset that works
 * un-reconnectable on exactly the firmwares this service now has to cope with. */
static u64 bta_profile_pick(const struct bta_profiles *p, const struct bt_devices *scanned) {
    u64 best = 0;
    const struct bta_profile *chosen = 0;
    for (unsigned i = 0; i < p->count; i++) {
        const struct bta_profile *e = &p->items[i];
        int present = 0;
        for (unsigned j = 0; j < scanned->count; j++)
            if (scanned->items[j].address == e->address) { present = 1; break; }
        if (!present) continue;
        if (!chosen || e->last_ok_us > chosen->last_ok_us ||
            (e->last_ok_us == chosen->last_ok_us && e->ok_count > chosen->ok_count)) {
            chosen = e;
            best = e->address;
        }
    }
    return best;
}
#endif
