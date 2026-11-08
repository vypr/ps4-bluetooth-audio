/* Host tests for the kernel resolver (app/kernel.h) and the six confirmed
 * signatures (app/kernel_sigs.h).
 *
 * They prove the matcher is correct (unique / none / ambiguous, wildcards), that
 * each shipped signature locates its own bytes and not another routine's, that the
 * fixed bytes agree with the accept-check and prologue constants, and - the point
 * of the refactor - that resolution DEGRADES per capability instead of refusing:
 * an unknown firmware still gets the patch, a missing or ambiguous signature only
 * clears its own feature. They compile with a plain `cc -I app -I tests`; nothing
 * here needs a PS4 toolchain. The one-firmware masks are not validation for any
 * other firmware (see docs/FIRMWARE.md) - only that firmware's own bytes can. */
#include "test.h"
#include <string.h>
#include <stdlib.h>
#include "../app/kernel.h"
#include "../app/fw14_slots.h"
#include "../app/kernel_sigs.h"

/* Lay a signature's own bytes into buf at off. Wildcard slots get filler that the
 * matcher must ignore; fixed slots get the real byte. */
static void place(unsigned char *buf, unsigned long off, const struct bta_sig *s) {
    for (unsigned j = 0; j < s->len; j++)
        buf[off + j] = s->mask[j] ? s->bytes[j] : (unsigned char)(0xA0 + j);
}
static const struct bta_kernel_sig *by_name(const char *name) {
    for (unsigned k = 0; k < BTA_KERNEL_SIG_COUNT; k++)
        if (!strcmp(bta_kernel_sigs[k].name, name)) return &bta_kernel_sigs[k];
    return 0;
}
static void write_le32(unsigned char *p, u32 v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}
static void build_sigs(struct bta_sig *dst) {
    for (unsigned i = 0; i < BTA_SIG_COUNT; i++) dst[i] = bta_kernel_sigs[i].sig;
}

/* A synthetic kernel: the six routine signatures at their real 13.52 offsets, with
 * fp_cache carrying the true cache displacements so the derivation has something real
 * to read. Only BTA_SIG_ROUTINES are placed: BTA_SIG_PATCHED describes the SAME
 * address as BTA_SIG_PATCH in its flipped form, so placing both would overwrite the
 * site. patch_kernel() below produces that state instead.
 * Caller frees. 0x5b8000 is just past fp_cache (+0x5b7570). */
#define KBUF 0x5b8000UL
static unsigned char *build_kernel(void) {
    unsigned char *k = calloc(KBUF, 1);
    if (!k) return 0;
    for (unsigned i = 0; i < BTA_SIG_ROUTINES; i++)
        place(k, bta_kernel_sigs[i].known_offset, &bta_kernel_sigs[i].sig);
    unsigned long m = bta_kernel_sigs[BTA_SIG_FPCACHE].known_offset;
    write_le32(k + m + 13, 0x18da8);   /* CACHE_FIRST */
    write_le32(k + m + 20, 0x20ea8);   /* CACHE_END */
    return k;
}
/* The same kernel as a restarted service meets it: the accept site already flipped,
 * so the original pattern no longer matches there and the patched one does. */
static void patch_kernel(unsigned char *k) {
    place(k, bta_kernel_sigs[BTA_SIG_PATCHED].known_offset, &bta_kernel_sigs[BTA_SIG_PATCHED].sig);
}

/* The resolve tests deliberately leave base 0, so a resolved address equals its known
 * 13.52 offset and the expectations stay readable. bta_kernel_format reads base only
 * to choose between "base ok" and "base not recovered", so lend it a real-looking one
 * for the report. The no-base case below calls bta_kernel_format directly. */
#define KBASE 0xffff800000000000UL
static unsigned report_line(struct bta_kernel *k, char *out, unsigned cap) {
    u64 saved = k->base;
    if (!k->base) k->base = KBASE;
    unsigned n = bta_kernel_format(k, out, cap);
    k->base = saved;
    return n;
}

void test_kernel(void) {
    /* The packet-type experiment must be exact-device, Active-only,
     * service-ready, one-slot, and handle-verified. */
    /* V7 protects the SpaceBuds experiment behind a build-time switch. */
    CHECK(bta_slots_observe_allowed(BTA_FW14_SLOTS_MAC));
    CHECK(bta_slots_observe_allowed(BTA_FW14_SPACESBUDS_MAC));
    CHECK(!bta_slots_observe_allowed(BTA_FW14_SPACESBUDS_MAC + 1));
    CHECK(bta_slots_command_target(BTA_FW14_SLOTS_MAC));
    CHECK_EQ(bta_slots_command_target(BTA_FW14_SPACESBUDS_MAC), BTA_V7_SPACEBUDS_RECOVER);
    CHECK(!bta_slots_command_target(BTA_FW14_SPACESBUDS_MAC + 1));
    CHECK_EQ(bta_slots_dispatch_allowed(BTA_FW14_SPACESBUDS_MAC, 0, 8, 10, 1, 2),
             BTA_V7_SPACEBUDS_RECOVER);
    CHECK(!bta_slots_dispatch_allowed(BTA_FW14_SPACESBUDS_MAC, 2, 8, 10, 1, 2));
    CHECK(!bta_slots_dispatch_allowed(BTA_FW14_SPACESBUDS_MAC, 0, 8, 10, 3, 2));
    CHECK(!bta_slots_dispatch_allowed(BTA_FW14_SPACESBUDS_MAC, 0, 8, 10, 1, 0));
    CHECK(bta_slots_dispatch_allowed(BTA_FW14_SLOTS_MAC, 0, 8, 10, 1, 2));
    CHECK(!bta_slots_dispatch_allowed(BTA_FW14_SLOTS_MAC + 1, 0, 8, 10, 1, 2));
    CHECK(!bta_slots_dispatch_allowed(BTA_FW14_SLOTS_MAC, 2, 8, 10, 1, 2));
    CHECK(!bta_slots_dispatch_allowed(BTA_FW14_SLOTS_MAC, 0, 0, 10, 1, 2));
    CHECK(!bta_slots_dispatch_allowed(BTA_FW14_SLOTS_MAC, 0, 8, 8, 1, 2));
    CHECK(!bta_slots_dispatch_allowed(BTA_FW14_SLOTS_MAC, 0, 8, 10, 3, 2));
    CHECK(!bta_slots_dispatch_allowed(BTA_FW14_SLOTS_MAC, 0, 8, 10, 1, 0));
    CHECK(bta_slots_wait_for_opportunity(BTA_SLOTS_WAIT_SERVICE));
    CHECK(bta_slots_wait_for_opportunity(BTA_SLOTS_WAIT_ACTIVE));
    CHECK(bta_slots_wait_for_opportunity(BTA_SLOTS_NO_LINK));
    CHECK(!bta_slots_wait_for_opportunity(BTA_SLOTS_SENT));
    CHECK(!bta_slots_wait_for_opportunity(BTA_SLOTS_COMMAND_ERROR));
    /* V5: per-link budget; 5s between links, 20s within the same link. */
    struct bta_slots_sample sample = {0};
    sample.status = BTA_SLOTS_OBSERVE;
    sample.channel = 10; sample.active = 8;
    sample.maxslots = 1; sample.handle = 2;
    sample.mode = 0;
    struct bta_slots_recovery_budget budget = {0};
    CHECK(bta_slots_recovery_narrow(&sample));
    CHECK(!bta_slots_recovery_ready(&sample, &budget, 0, 1, 100000000UL));
    CHECK(bta_slots_recovery_ready(&sample, &budget, 0, 2, 100000000UL));
    bta_slots_recovery_record_request(&budget, 100000000UL);
    CHECK_EQ(budget.requests, 1);
    CHECK_EQ(budget.ever_issued, 1);
    CHECK_EQ(budget.last_request_at, 100000000UL);
    CHECK(!bta_slots_recovery_ready(&sample, &budget, 0, 2, 130000000UL));
    CHECK(!bta_slots_recovery_ready(&sample, &budget, 1, 2, 119000000UL));
    CHECK(bta_slots_recovery_ready(&sample, &budget, 1, 2, 120000000UL));
    bta_slots_recovery_record_request(&budget, 120000000UL);
    CHECK(!bta_slots_recovery_ready(&sample, &budget, 0, 2, 160000000UL));
    CHECK(bta_slots_recovery_ready(&sample, &budget, 1, 2, 160000000UL));
    bta_slots_recovery_record_request(&budget, 160000000UL);
    CHECK_EQ(budget.requests, BTA_SLOTS_SESSION_CAP);
    CHECK(!bta_slots_recovery_ready(&sample, &budget, 1, 2, 190000000UL));
    bta_slots_recovery_reset_link(&budget);  /* real link teardown */
    CHECK_EQ(budget.requests, 0);
    CHECK_EQ(budget.ever_issued, 1);          /* last command still matters */
    CHECK_EQ(budget.last_request_at, 160000000UL);
    CHECK(!bta_slots_recovery_ready(&sample, &budget, 0, 2, 164000000UL));
    CHECK(bta_slots_recovery_ready(&sample, &budget, 0, 2, 165000000UL));
    bta_slots_recovery_record_request(&budget, 165000000UL);
    CHECK_EQ(budget.requests, 1);
    CHECK(!bta_slots_recovery_ready(&sample, &budget, 1, 2, 184000000UL));
    CHECK(bta_slots_recovery_ready(&sample, &budget, 1, 2, 185000000UL));
    CHECK(!bta_slots_recovery_ready(0, &budget, 1, 2, 200000000UL));
    CHECK(!bta_slots_recovery_ready(&sample, 0, 1, 2, 200000000UL));
    sample.mode = 2;
    CHECK(bta_slots_recovery_narrow(&sample)); /* Sniff can count, not send */
    CHECK(!bta_slots_recovery_ready(&sample, &budget, 1, 2, 220000000UL));
    sample.mode = 0; sample.maxslots = 3;
    CHECK(!bta_slots_recovery_narrow(&sample));
    CHECK(!bta_slots_recovery_ready(&sample, &budget, 1, 2, 220000000UL));
    sample.maxslots = 1; sample.channel = 8;
    CHECK(!bta_slots_recovery_narrow(&sample));
    sample.channel = 10; sample.active = 0;
    CHECK(!bta_slots_recovery_narrow(&sample));
    sample.active = 8; sample.handle = 0;
    CHECK(!bta_slots_recovery_narrow(&sample));
    unsigned char buf[256];

    /* --- matcher mechanics on a tiny hand-built signature (slot 1 wildcarded) --- */
    struct bta_sig t = { 4, {0xde, 0x00, 0xbe, 0xef}, {0xff, 0x00, 0xff, 0xff} };
    memset(buf, 0, sizeof(buf));
    buf[10] = 0xde; buf[11] = 0x99; buf[12] = 0xbe; buf[13] = 0xef;
    CHECK_EQ(bta_sig_scan(buf, sizeof(buf), &t), 10);
    buf[11] = 0x00;                                  /* wildcard slot: still matches */
    CHECK_EQ(bta_sig_scan(buf, sizeof(buf), &t), 10);
    buf[12] = 0x00;                                  /* break a fixed byte: gone */
    CHECK_EQ(bta_sig_scan(buf, sizeof(buf), &t), -1);
    memset(buf, 0, sizeof(buf));                     /* two matches -> ambiguous */
    buf[10] = 0xde; buf[11] = 0x11; buf[12] = 0xbe; buf[13] = 0xef;
    buf[40] = 0xde; buf[41] = 0x22; buf[42] = 0xbe; buf[43] = 0xef;
    CHECK_EQ(bta_sig_scan(buf, sizeof(buf), &t), -2);
    CHECK_EQ(bta_sig_scan(buf, 3, &t), -1);          /* buffer shorter than the sig */

    /* --- each shipped signature locates its own bytes, uniquely --- */
    CHECK_EQ(BTA_SIG_ROUTINES, 6);                        /* six routines are reported on */
    CHECK_EQ(BTA_KERNEL_SIG_COUNT, BTA_SIG_COUNT);        /* plus the patched form = swept */
    CHECK_EQ(BTA_SIG_COUNT, BTA_SIG_ROUTINES + 1);
    for (unsigned k = 0; k < BTA_KERNEL_SIG_COUNT; k++) {
        const struct bta_sig *s = &bta_kernel_sigs[k].sig;
        unsigned char big[1024];
        const unsigned long off = 300;
        CHECK(s->len >= 16u && s->len <= BTA_SIG_MAX);
        memset(big, 0, sizeof(big));
        place(big, off, s);
        CHECK_EQ(bta_sig_scan(big, sizeof(big), s), (long)off);
        big[off] = (unsigned char)(big[off] ^ 0xff);   /* first byte is fixed: break it */
        CHECK_EQ(bta_sig_scan(big, sizeof(big), s), -1);
        CHECK(bta_kernel_sigs[k].known_offset != 0);
    }

    /* --- signatures are specific: one routine's bytes don't match another's sig,
     *     including command vs fp_cache, which share a prologue then diverge. --- */
    {
        const struct bta_kernel_sig *lock = by_name("bt_lock"), *unlock = by_name("bt_unlock");
        const struct bta_kernel_sig *cmd = by_name("command"), *cache = by_name("fp_cache");
        unsigned char b2[512];
        CHECK(lock && unlock && cmd && cache);
        memset(b2, 0, sizeof(b2)); place(b2, 100, &lock->sig);
        CHECK_EQ(bta_sig_scan(b2, sizeof(b2), &unlock->sig), -1);
        memset(b2, 0, sizeof(b2)); place(b2, 100, &cmd->sig);
        CHECK_EQ(bta_sig_scan(b2, sizeof(b2), &cache->sig), -1);
        memset(b2, 0, sizeof(b2)); place(b2, 100, &cache->sig);
        CHECK_EQ(bta_sig_scan(b2, sizeof(b2), &cmd->sig), -1);
    }

    /* --- the fixed bytes agree with the accept-check and prologue constants --- */
    {
        const struct bta_kernel_sig *patch = by_name("patch_site");
        const struct bta_kernel_sig *cmd = by_name("command"), *cache = by_name("fp_cache");
        CHECK(patch && cmd && cache);
        for (unsigned j = 0; j < 8; j++) {
            CHECK_EQ(patch->sig.bytes[j], (unsigned char)(BTA_ACCEPT_ORIGINAL >> (8 * j)));
            CHECK_EQ(patch->sig.mask[j], 0xff);
            CHECK_EQ(cmd->sig.bytes[j], (unsigned char)(BTA_PROLOGUE >> (8 * j)));
            CHECK_EQ(cache->sig.bytes[j], (unsigned char)(BTA_PROLOGUE >> (8 * j)));
        }
    }

    struct bta_sig sigs[BTA_SIG_COUNT];
    build_sigs(sigs);

    /* --- all six resolve: every address lands on its 13.52 offset, the cache layout
     *     derives from fp_cache's operands, the known-firmware table applies, and
     *     all three capabilities light up. (base 0, so absolute == offset.) --- */
    {
        unsigned char *k = build_kernel();
        CHECK(k != 0);
        if (k) {
            struct bta_kernel kern;
            memset(&kern, 0, sizeof(kern));
            kern.fw_version = BTA_FW_13520001;
            bta_kernel_resolve(&kern, k, KBUF, sigs, BTA_SIG_COUNT);
            for (unsigned i = 0; i < BTA_SIG_ROUTINES; i++) CHECK_EQ(kern.sig_flag[i], 0);
            CHECK_EQ(kern.copyout,    (long)by_name("copyout")->known_offset);
            CHECK_EQ(kern.lock,       (long)by_name("bt_lock")->known_offset);
            CHECK_EQ(kern.unlock,     (long)by_name("bt_unlock")->known_offset);
            CHECK_EQ(kern.command,    (long)by_name("command")->known_offset);
            CHECK_EQ(kern.patch_site, (long)by_name("patch_site")->known_offset);
            CHECK_EQ(kern.fp_cache,   (long)by_name("fp_cache")->known_offset);
            /* the cache layout falls out of the code, not a guess */
            CHECK_EQ(kern.cache_first, 0x18da8);
            CHECK_EQ(kern.cache_end,   0x20ea8);
            /* the known-firmware table applied (spot-check) */
            CHECK_EQ(kern.ctx_ptr,    0x2612550);
            CHECK_EQ(kern.fmt_string, 0xad7562);
            CHECK_EQ(kern.cache_stride, 0x408);
            CHECK_EQ(kern.dev_name,   0x80);
            CHECK_EQ(kern.link_stride, 0x1588);
            CHECK_EQ(kern.caps, BTA_KCAP_PATCH | BTA_KCAP_DEVLIST | BTA_KCAP_WIDEN);

            char line[160];
            unsigned n = report_line(&kern, line, sizeof(line));
            CHECK(n == strlen(line));
            CHECK(strstr(line, "base ok") != 0);
            CHECK(strstr(line, "6/6 signatures") != 0);
            CHECK(strstr(line, "fw 0x13520001") != 0);
            CHECK(strstr(line, "patch+devlist+widen") != 0);
            free(k);
        }
    }

    /* --- the cache bounds are sanity-checked: garbage displacements leave DEVLIST
     *     clear while the code-independent capabilities stay. --- */
    {
        unsigned long m = bta_kernel_sigs[BTA_SIG_FPCACHE].known_offset;
        unsigned char *k = build_kernel();
        CHECK(k != 0);
        if (k) {
            struct bta_kernel kern;
            /* end < first: rejected */
            write_le32(k + m + 13, 0x20ea8); write_le32(k + m + 20, 0x18da8);
            memset(&kern, 0, sizeof(kern)); kern.fw_version = BTA_FW_13520001;
            bta_kernel_resolve(&kern, k, KBUF, sigs, BTA_SIG_COUNT);
            CHECK_EQ(kern.cache_first, 0x20ea8);          /* still read from the code */
            CHECK_EQ(kern.cache_end,   0x18da8);
            CHECK_EQ(kern.caps & BTA_KCAP_DEVLIST, 0u);   /* but not trusted */
            CHECK(kern.caps & BTA_KCAP_PATCH);
            CHECK(kern.caps & BTA_KCAP_WIDEN);
            /* difference not a whole number of 0x408-byte records: rejected */
            write_le32(k + m + 13, 0x18da8); write_le32(k + m + 20, 0x18da8 + 0x409);
            memset(&kern, 0, sizeof(kern)); kern.fw_version = BTA_FW_13520001;
            bta_kernel_resolve(&kern, k, KBUF, sigs, BTA_SIG_COUNT);
            CHECK_EQ(kern.caps & BTA_KCAP_DEVLIST, 0u);
            free(k);
        }
    }

    /* --- degradation, not refusal: one missing signature clears only its feature. --- */
    {
        unsigned char *k = build_kernel();
        CHECK(k != 0);
        if (k) {
            struct bta_kernel kern;
            unsigned long po = by_name("patch_site")->known_offset;
            unsigned long fo = by_name("fp_cache")->known_offset;
            unsigned long co = by_name("command")->known_offset;
            unsigned char save;

            save = k[po]; k[po] ^= 0xff;                  /* patch_site gone */
            memset(&kern, 0, sizeof(kern)); kern.fw_version = BTA_FW_13520001;
            bta_kernel_resolve(&kern, k, KBUF, sigs, BTA_SIG_COUNT);
            CHECK_EQ(kern.sig_flag[BTA_SIG_PATCH], 1);
            CHECK_EQ(kern.patch_site, 0);
            CHECK_EQ(kern.caps & BTA_KCAP_PATCH, 0u);
            CHECK(kern.caps & BTA_KCAP_DEVLIST);          /* the others survive */
            CHECK(kern.caps & BTA_KCAP_WIDEN);
            k[po] = save;

            save = k[fo]; k[fo] ^= 0xff;                  /* fp_cache gone -> no cache bounds */
            memset(&kern, 0, sizeof(kern)); kern.fw_version = BTA_FW_13520001;
            bta_kernel_resolve(&kern, k, KBUF, sigs, BTA_SIG_COUNT);
            CHECK_EQ(kern.caps & BTA_KCAP_DEVLIST, 0u);
            CHECK(kern.caps & BTA_KCAP_PATCH);
            CHECK(kern.caps & BTA_KCAP_WIDEN);
            k[fo] = save;

            save = k[co]; k[co] ^= 0xff;                  /* command gone -> no widen */
            memset(&kern, 0, sizeof(kern)); kern.fw_version = BTA_FW_13520001;
            bta_kernel_resolve(&kern, k, KBUF, sigs, BTA_SIG_COUNT);
            CHECK_EQ(kern.caps & BTA_KCAP_WIDEN, 0u);
            CHECK(kern.caps & BTA_KCAP_PATCH);
            CHECK(kern.caps & BTA_KCAP_DEVLIST);
            k[co] = save;
        }
    }

    /* --- a duplicated signature is flagged ambiguous (2) and clears its feature. --- */
    {
        unsigned char *k = build_kernel();
        CHECK(k != 0);
        if (k) {
            struct bta_kernel kern;
            place(k, 0x100000, &by_name("patch_site")->sig);  /* a second patch_site */
            memset(&kern, 0, sizeof(kern)); kern.fw_version = BTA_FW_13520001;
            bta_kernel_resolve(&kern, k, KBUF, sigs, BTA_SIG_COUNT);
            CHECK_EQ(kern.sig_flag[BTA_SIG_PATCH], 2);
            CHECK_EQ(kern.patch_site, 0);
            CHECK_EQ(kern.caps & BTA_KCAP_PATCH, 0u);
            CHECK(kern.caps & BTA_KCAP_DEVLIST);
            char line[160];
            report_line(&kern, line, sizeof(line));
            CHECK(strstr(line, "5/6 signatures") != 0);
            CHECK(strstr(line, "patch_site ambiguous") != 0);
            CHECK(strstr(line, "devlist+widen") != 0);
            free(k);
        }
    }

    /* --- THE refactor's reason to exist: an unknown firmware still resolves every
     *     signature and KEEPS the patch; only the table-dependent features go dark. --- */
    {
        unsigned char *k = build_kernel();
        CHECK(k != 0);
        if (k) {
            struct bta_kernel kern;
            memset(&kern, 0, sizeof(kern));
            kern.fw_version = 0x09000001;                 /* a firmware we have no table for */
            bta_kernel_resolve(&kern, k, KBUF, sigs, BTA_SIG_COUNT);
            for (unsigned i = 0; i < BTA_SIG_ROUTINES; i++) CHECK_EQ(kern.sig_flag[i], 0);
            CHECK(kern.patch_site != 0);
            CHECK_EQ(kern.ctx_ptr, 0);
            CHECK_EQ(kern.fmt_string, 0);
            CHECK_EQ(kern.cache_stride, 0);
            CHECK_EQ(kern.caps, BTA_KCAP_PATCH);          /* patch only */
            char line[160];
            report_line(&kern, line, sizeof(line));
            CHECK(strstr(line, "base ok") != 0);
            CHECK(strstr(line, "6/6 signatures") != 0);
            CHECK(strstr(line, "fw 0x09000001") != 0);
            CHECK(strstr(line, "patch only") != 0);
            free(k);
        }
    }

    /* --- a restarted service meeting its own live patch (no reboot in between: the
     *     app relaunches the service, deploy_variant.py stops and resends it). The
     *     accept site's own bytes are flipped, so the original pattern cannot match
     *     there; the service must recognise the patched form and still report the
     *     patch as working, not as a missing routine. --- */
    {
        unsigned char *k = build_kernel();
        CHECK(k != 0);
        if (k) {
            patch_kernel(k);
            struct bta_kernel kern;
            memset(&kern, 0, sizeof(kern));
            kern.fw_version = 0x09000001;             /* an untested firmware, as above */
            bta_kernel_resolve(&kern, k, KBUF, sigs, BTA_SIG_COUNT);
            CHECK_EQ(kern.sig_flag[BTA_SIG_PATCH], 1);    /* the original form is gone... */
            CHECK_EQ(kern.sig_flag[BTA_SIG_PATCHED], 0);  /* ...because it is patched */
            CHECK_EQ(kern.patch_site, 0);
            CHECK_EQ(kern.patch_live, (long)by_name("patch_site")->known_offset);
            CHECK_EQ(kern.caps, BTA_KCAP_PATCH);          /* still usable: that is the point */
            char line[160];
            report_line(&kern, line, sizeof(line));
            CHECK(strstr(line, "patch_site already patched") != 0);
            CHECK(strstr(line, "not found") == 0);        /* never reported as missing */
            CHECK(strstr(line, "patch only") != 0);
            free(k);
        }
    }

    /* --- no base recovered: the report says so and names the firmware. --- */
    {
        struct bta_kernel kern;
        memset(&kern, 0, sizeof(kern));
        kern.fw_version = 0x05050001;
        char line[160];
        bta_kernel_format(&kern, line, sizeof(line));
        CHECK(strstr(line, "not recovered") != 0);
        CHECK(strstr(line, "fw 0x05050001") != 0);
    }
}
