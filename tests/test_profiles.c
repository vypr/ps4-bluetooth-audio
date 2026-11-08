#include "test.h"
#include <string.h>
#include "profiles.h"

/* An audio device (service bit 3) with a given address and name. */
static struct bt_device dev(u64 address, const char *name) {
    struct bt_device d;
    memset(&d, 0, sizeof(d));
    d.address = address;
    d.services = 8;
    d.cod = 0x240404;
    for (unsigned i = 0; name[i] && i < sizeof(d.name) - 1; i++) d.name[i] = name[i];
    return d;
}

void test_profiles(void) {
    struct bta_profiles p;
    bta_profiles_init(&p);
    CHECK_EQ(p.count, 0);
    CHECK_EQ(bta_profiles_valid(&p, sizeof(p)), 1);
    CHECK_EQ(bta_profiles_valid(&p, sizeof(p) - 1), 0); /* wrong size */
    p.version = 999;
    CHECK_EQ(bta_profiles_valid(&p, sizeof(p)), 0);     /* wrong version */
    p.version = BTA_PROFILES_VERSION;

    /* Touch adds a device, defaults to joint, and finds it again. */
    struct bt_device airpods = dev(0x1122334455, "AirPods Pro");
    int i = bta_profile_touch(&p, &airpods);
    CHECK_EQ(i, 0);
    CHECK_EQ(p.count, 1);
    CHECK_EQ(bta_profile_find(&p, airpods.address), 0);
    CHECK_EQ(bta_profile_find(&p, 0x999), -1);
    CHECK_EQ(bta_profile_mode(&p, airpods.address), SBC_JOINT);
    CHECK_EQ(bta_profile_mode(&p, 0x999), SBC_JOINT); /* default for unknown */
    CHECK_EQ(bta_profile_known(&p, airpods.address), 0); /* selected, never played */
    CHECK_EQ(bta_profile_known(&p, 0x999), 0);
    struct bta_profiles tried = p;
    bta_profile_record_fail(&tried, airpods.address);
    CHECK_EQ(bta_profile_known(&tried, airpods.address), 0); /* a failed try still isn't */

    /* Touching again updates the name/class but keeps the slot and counts. */
    bta_profile_record_ok(&p, airpods.address, SBC_JOINT, 1000);
    CHECK_EQ(bta_profile_known(&p, airpods.address), 1); /* played sound once */
    struct bt_device renamed = dev(0x1122334455, "S's AirPods");
    CHECK_EQ(bta_profile_touch(&p, &renamed), 0);
    CHECK_EQ(p.count, 1);
    CHECK_EQ(p.items[0].ok_count, 1);
    CHECK(p.items[0].name[0] == 'S');

    /* record_ok remembers the working mode and bumps the success count/time. */
    bta_profile_record_ok(&p, airpods.address, SBC_DUAL, 5000);
    CHECK_EQ(bta_profile_mode(&p, airpods.address), SBC_DUAL);
    CHECK_EQ(p.items[0].ok_count, 2);
    CHECK_EQ(p.items[0].last_ok_us, 5000);
    bta_profile_record_fail(&p, airpods.address);
    CHECK_EQ(p.items[0].fail_count, 1);

    /* set_mode changes the preference only: counts and recency stay put. */
    bta_profile_set_mode(&p, airpods.address, SBC_JOINT);
    CHECK_EQ(bta_profile_mode(&p, airpods.address), SBC_JOINT);
    CHECK_EQ(p.items[0].ok_count, 2);
    CHECK_EQ(p.items[0].last_ok_us, 5000);
    bta_profile_set_mode(&p, 0x999, SBC_DUAL); /* unknown: no-op, no crash */

    /* A second device. pick() returns the most-recently-used present one. */
    struct bt_device jbl = dev(0xAABBCCDD, "JBL Flip");
    bta_profile_touch(&p, &jbl);
    bta_profile_record_ok(&p, jbl.address, SBC_JOINT, 9000); /* JBL more recent */
    CHECK_EQ(p.count, 2);

    struct bt_devices scan;
    memset(&scan, 0, sizeof(scan));
    scan.items[0] = airpods; scan.items[1] = jbl; scan.count = 2;
    CHECK_EQ(bta_profile_pick(&p, &scan), jbl.address); /* both present -> newest */

    scan.count = 1; scan.items[0] = airpods;           /* only AirPods in range */
    CHECK_EQ(bta_profile_pick(&p, &scan), airpods.address);

    struct bt_devices empty;
    memset(&empty, 0, sizeof(empty));
    CHECK_EQ(bta_profile_pick(&p, &empty), 0);          /* none in range */

    /* Presence is address-only: a remembered device is reconnected even when the
     * class of device the list carries says it is not audio. It earned its place in
     * the table by playing sound, and on a firmware without the driver's device
     * cache the list comes from scan events, which have no service bits at all. */
    struct bt_device odd = dev(0x1122334455, "AirPods Pro");
    odd.services = 0; odd.cod = 0x002580; /* reports HID, not audio */
    struct bt_devices oddlist;
    memset(&oddlist, 0, sizeof(oddlist));
    oddlist.items[0] = odd; oddlist.count = 1;
    CHECK_EQ(bt_is_audio(&odd), 0);                              /* it really does not look like audio */
    CHECK_EQ(bta_profile_pick(&p, &oddlist), odd.address);       /* ...and is still picked */

    /* An address that was never remembered is still not picked. */
    struct bt_device stranger = dev(0x99aabbccddee, "Someone else");
    struct bt_devices unknown;
    memset(&unknown, 0, sizeof(unknown));
    unknown.items[0] = stranger; unknown.count = 1;
    CHECK_EQ(bta_profile_pick(&p, &unknown), 0);

    /* bta_profile_device reconstructs a usable target from a profile. */
    struct bt_device t;
    bta_profile_device(&p.items[1], &t);
    CHECK_EQ(t.address, jbl.address);
    CHECK_EQ(bt_is_audio(&t), 1);
    CHECK(t.name[0] == 'J');

    /* Eviction: fill the table, then a new device replaces the least-recently
     * used (lowest last_ok_us). items[0] AirPods is oldest working (5000). */
    bta_profiles_init(&p);
    for (unsigned k = 0; k < BTA_PROFILE_MAX; k++) {
        struct bt_device d = dev(0x100 + k, "d");
        bta_profile_touch(&p, &d);
        bta_profile_record_ok(&p, d.address, SBC_JOINT, 1000 + k); /* k=0 oldest */
    }
    CHECK_EQ(p.count, BTA_PROFILE_MAX);
    struct bt_device extra = dev(0x900, "new");
    bta_profile_touch(&p, &extra);
    CHECK_EQ(p.count, BTA_PROFILE_MAX);               /* still full */
    CHECK_EQ(bta_profile_find(&p, 0x100), -1);         /* oldest evicted */
    CHECK(bta_profile_find(&p, 0x900) >= 0);           /* newcomer kept */
}
