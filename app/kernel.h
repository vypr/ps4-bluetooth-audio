#ifndef BTA_KERNEL_H
#define BTA_KERNEL_H
/* Runtime kernel resolution. Instead of trusting a per-firmware offset table, the
 * service locates the kernel routines it needs by their own code signatures at
 * startup and derives what it can from the matched code. Nothing here gates on a
 * firmware version: whatever resolves is used, whatever does not resolve turns its
 * feature off. Worst case on an unknown kernel is a feature that stays dark, never
 * a wrong write. The matcher is app/sigscan.h; the signatures are app/kernel_sigs.h.
 *
 * Public API (the resolver shell and the three service entry points live in
 * app/bt_patch.c; everything in THIS header is pure and host-testable):
 *
 *   int bta_kernel_init(void);
 *       Resolve once in ring 0 and cache the result in a userland global. Returns 0
 *       when the kernel base was recovered (and the result copied out), nonzero
 *       otherwise. Idempotent: later calls return the cached verdict. The three
 *       feature entry points (bta_apply_patch / bt_list / bt_prepare) call it
 *       themselves, so callers need not sequence it.
 *   unsigned bta_kernel_caps(void);        BTA_KCAP_* bitmask of what resolved.
 *   const struct bta_kernel *bta_kernel_info(void);   the cached resolution.
 *   unsigned bta_kernel_report(char *out, unsigned cap);
 *       One human-readable line describing the resolution (for a bug report);
 *       returns its length. Formatting is bta_kernel_format() below.
 *
 * What resolves generically (any firmware) vs. from the known-firmware table:
 *   - The six routine addresses and the cache bounds are found/derived from live
 *     code, so they resolve on any kernel whose code still matches the signatures.
 *   - ctx_ptr, fmt_string and the record-layout offsets cannot be found by pattern
 *     (a data pointer in a zeroed region; a string run; struct field offsets), so
 *     they come from a small table applied ONLY when the firmware is 13.520.001.
 *     On any other firmware they stay zero and the device-list / widen features
 *     stay off; the accept patch, which needs none of them, still works. */
#include "bta_types.h"
#include "sigscan.h"

/* Capabilities. Each is independent: one clearing does not clear the others. */
#define BTA_KCAP_PATCH    1u  /* patch_site resolved: the accept patch can be applied */
#define BTA_KCAP_DEVLIST  2u  /* device cache readable: real names and service bits */
#define BTA_KCAP_WIDEN    4u  /* HCI three-slot widening available */

/* Index of each signature in bta_kernel.sig_flag[] and in the table passed to the
 * scanner. kernel_sigs.h lays the signatures out in this order and asserts it.
 *
 * BTA_SIG_PATCHED is patch_site in its already-flipped form, not a seventh routine:
 * the patch_site pattern opens with the bytes the patch overwrites, so once the
 * patch is live that pattern stops matching. The service restarts far more often
 * than the console reboots (the app relaunches it, deploy_variant.py stops and
 * resends it), and the patch is RAM-only, so a restarted service meets its own
 * patch constantly. Without this it would report the site as missing while the
 * patch is in fact working. Hence the report counts BTA_SIG_ROUTINES, not
 * BTA_SIG_COUNT: exactly one of the two patch forms can match. */
enum {
    BTA_SIG_COPYOUT, BTA_SIG_LOCK, BTA_SIG_UNLOCK,
    BTA_SIG_COMMAND, BTA_SIG_PATCH, BTA_SIG_FPCACHE,
    BTA_SIG_ROUTINES,                   /* the six routines the report counts */
    BTA_SIG_PATCHED = BTA_SIG_ROUTINES, /* alternative form of BTA_SIG_PATCH */
    BTA_SIG_COUNT                       /* everything the scanner sweeps for */
};

/* The accept check the patch flips, and its replacement. The patch_site signature
 * opens with ACCEPT_ORIGINAL's bytes, so a matched signature already proves the
 * live bytes; the patch re-verifies them before the flip regardless. Both forms are
 * firmware-independent given the signature matched, so the patch needs no table.
 *   cmp dword [rax+0x20],0x40; jne +0x2a  ->  test byte [rax+0x20],0x48; je +0x2a */
#define BTA_ACCEPT_ORIGINAL    0x8b492a7540207883UL
#define BTA_ACCEPT_REPLACEMENT 0x8b492a74482040f6UL
/* Shared prologue of command and fp_cache (push rbp; mov rbp,rsp; push r15; push r14).
 * The host tests pin those two signatures' opening bytes to this value. */
#define BTA_PROLOGUE           0x56415741e5894855UL

/* The one firmware we have the non-code values for. sceKernelGetSystemSwVersion
 * reports the firmware as 0xMMMMNNNN. */
#define BTA_FW_13520001 0x13520001u
#define BTA_FW_14008001 0x14008001u

struct bta_kernel {
    u64 base;
    /* located by signature; absolute (base + match offset), 0 = unresolved */
    u64 copyout, lock, unlock, command, patch_site, fp_cache;
    /* the accept site found already flipped: the patch is live from an earlier run
     * of this service in the same jailbreak. Set instead of patch_site, never both. */
    u64 patch_live;
    /* known-firmware table, absolute; 0 = unknown firmware */
    u64 ctx_ptr, fmt_string;
    /* classic device cache bounds, read out of fp_cache's own operands */
    u32 cache_first, cache_end;
    /* known-firmware record layout; 0 on any firmware but 13.520.001 */
    u32 cache_stride;
    u32 dev_cod, dev_services, dev_flags, dev_name;
    u32 link_first, link_end, link_stride;
    u32 link_address, link_info, link_handle, link_pkt_count, link_pkt_types;
    u32 cmd_set_pkt_type, pkt_types_3slot;
    u32 fw_version;          /* as sceKernelGetSystemSwVersion reports it */
    u8  sig_flag[BTA_SIG_COUNT]; /* per signature: 0 one match, 1 none, 2 ambiguous */
    u32 caps;
};

/* Little-endian u32 from a byte run (the fp_cache disp32 operands). Avoids an
 * unaligned struct deref and reads the same on host and target. */
static inline u32 bta_rd32(const unsigned char *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* One sweep over [buf, buf+len): at every position test all nsig signatures, so the
 * kernel text is streamed once rather than nsig times. Fills sig_flag[], the six
 * absolute addresses (base already set in k), and the cache bounds. k->base and the
 * fields this does not touch must be set/zeroed by the caller. Pure: no syscalls,
 * no libc, no payload globals - safe to inline into a ring-0 callback. */
static inline void bta_kernel_scan(struct bta_kernel *k, const unsigned char *buf,
                                   unsigned long len, const struct bta_sig *sigs,
                                   unsigned nsig) {
    unsigned long off[BTA_SIG_COUNT];
    if (nsig > BTA_SIG_COUNT) nsig = BTA_SIG_COUNT;
    for (unsigned s = 0; s < nsig; s++) { k->sig_flag[s] = 1; off[s] = 0; } /* 1 = none yet */
    for (unsigned long i = 0; i < len; i++) {
        unsigned long avail = len - i;
        for (unsigned s = 0; s < nsig; s++) {
            if (k->sig_flag[s] == 2) continue;              /* already ambiguous */
            if (!bta_sig_match(buf + i, avail, &sigs[s])) continue;
            if (k->sig_flag[s] == 0) k->sig_flag[s] = 2;    /* a second match */
            else { k->sig_flag[s] = 0; off[s] = i; }        /* the first match */
        }
    }
    /* Resolved addresses are absolute. A single match (flag 0) resolves; none (1)
     * or ambiguous (2) leaves the address 0 so compute_caps turns the feature off. */
    k->copyout    = k->sig_flag[BTA_SIG_COPYOUT] == 0 ? k->base + off[BTA_SIG_COPYOUT] : 0;
    k->lock       = k->sig_flag[BTA_SIG_LOCK]    == 0 ? k->base + off[BTA_SIG_LOCK]    : 0;
    k->unlock     = k->sig_flag[BTA_SIG_UNLOCK]  == 0 ? k->base + off[BTA_SIG_UNLOCK]  : 0;
    k->command    = k->sig_flag[BTA_SIG_COMMAND] == 0 ? k->base + off[BTA_SIG_COMMAND] : 0;
    k->patch_site = k->sig_flag[BTA_SIG_PATCH]   == 0 ? k->base + off[BTA_SIG_PATCH]   : 0;
    k->fp_cache   = k->sig_flag[BTA_SIG_FPCACHE] == 0 ? k->base + off[BTA_SIG_FPCACHE] : 0;
    k->patch_live = k->sig_flag[BTA_SIG_PATCHED] == 0 ? k->base + off[BTA_SIG_PATCHED] : 0;
    /* The cache layout falls out of fp_cache's own operands:
     *   idx 10..12 lea rax,[rdi+disp32]; idx 13 = CACHE_FIRST (disp32)
     *   idx 17..19 lea rcx,[rdi+disp32]; idx 20 = CACHE_END   (disp32)
     * read from the code, not guessed, so the bounds are right on any build that
     * still carries this prologue. The match fits in the window (sig len 33 > 24). */
    if (k->sig_flag[BTA_SIG_FPCACHE] == 0) {
        unsigned long m = off[BTA_SIG_FPCACHE];
        k->cache_first = bta_rd32(buf + m + 13);
        k->cache_end   = bta_rd32(buf + m + 20);
    }
}

/* The two addresses and the record layout that cannot be found by signature. Never
 * a gate: applied only on the one firmware we measured them on, left zero otherwise
 * so the features that need them stay off on every other kernel. */
static inline void bta_kernel_apply_fw_table(struct bta_kernel *k) {
    if (k->fw_version == BTA_FW_14008001) {
        /* FW14: verified context, cache, COD, services and name.
         * Keep WIDEN disabled until the link layout is validated. */
        k->ctx_ptr      = k->base + 0x2612550UL;
        k->cache_stride = 0x408;
        k->dev_cod      = 0x08;
        k->dev_services = 0x20;
        k->dev_flags    = 0x24; /* provisional field interpretation */
        k->dev_name     = 0x80;
        return;
    }
    if (k->fw_version != BTA_FW_13520001) return;
    k->ctx_ptr        = k->base + 0x2612550UL; /* void** holding the Bluetooth context */
    k->fmt_string     = k->base + 0xad7562UL;  /* format string command() expects */
    k->cache_stride   = 0x408;
    k->dev_cod = 8; k->dev_services = 0x20; k->dev_flags = 0x24; k->dev_name = 0x80;
    k->link_first = 0x42a8; k->link_end = 0xd960; k->link_stride = 0x1588;
    k->link_address = 0x30; k->link_info = 0x638; k->link_handle = 0x48;
    k->link_pkt_count = 0x1560; k->link_pkt_types = 0x1564;
    k->cmd_set_pkt_type = 0x40f; k->pkt_types_3slot = 0x3c18;
}

/* Which features are available, from what resolved. Each capability is independent.
 * PATCH is generic (patch_site + copyout). DEVLIST and WIDEN also need the
 * known-firmware table (ctx_ptr etc.), so they are only ever set on 13.520.001. */
static inline void bta_kernel_compute_caps(struct bta_kernel *k) {
    unsigned caps = 0;
    /* Either form of the site counts: one says the patch can be applied, the other
     * says it already is. Both mean audio-class devices are admitted. */
    if ((k->patch_site || k->patch_live) && k->copyout) caps |= BTA_KCAP_PATCH;
    int core = k->copyout && k->lock && k->unlock;
    /* Cache bounds must be sane before we walk them: ordered, bounded, and an exact
     * number of 0x408-byte records apart (the classic cache stride). */
    int cache_ok = k->fp_cache && k->cache_end > k->cache_first
                && k->cache_first < 0x100000u && k->cache_end < 0x100000u
                && ((k->cache_end - k->cache_first) % 0x408u) == 0;
    if (core && cache_ok && k->ctx_ptr && k->cache_stride) caps |= BTA_KCAP_DEVLIST;
    if (core && k->command && k->ctx_ptr && k->fmt_string) caps |= BTA_KCAP_WIDEN;
    k->caps = caps;
}

/* Full resolution over a buffer: scan, apply the known-firmware table, compute caps.
 * k->base and k->fw_version must be set and everything else zeroed before the call.
 * Pure (no syscalls/libc/globals), so the ring-0 shell and the host tests share it. */
static inline void bta_kernel_resolve(struct bta_kernel *k, const unsigned char *buf,
                                      unsigned long len, const struct bta_sig *sigs,
                                      unsigned nsig) {
    bta_kernel_scan(k, buf, len, sigs, nsig);
    bta_kernel_apply_fw_table(k);
    bta_kernel_compute_caps(k);
}

/* --- report line (pure; no libc) ------------------------------------------------ */
struct bta_kw { char *o; unsigned n, cap; };
static inline void bta_kw_s(struct bta_kw *w, const char *s) {
    while (*s && w->n + 1 < w->cap) w->o[w->n++] = *s++;
}
static inline void bta_kw_c(struct bta_kw *w, char c) { if (w->n + 1 < w->cap) w->o[w->n++] = c; }
static inline void bta_kw_x(struct bta_kw *w, u64 v, int digits) {
    const char *h = "0123456789abcdef";
    bta_kw_c(w, '0'); bta_kw_c(w, 'x');
    for (int i = (digits - 1) * 4; i >= 0; i -= 4) bta_kw_c(w, h[(v >> i) & 0xf]);
}
/* e.g. "kernel: base ok, 6/6 signatures, fw 0x13520001, patch+devlist+widen" or
 *      "kernel: base ok, 5/6 signatures (command ambiguous), fw 0x09000001, patch only"
 * NUL-terminates out and returns the length written (never exceeding cap-1). */
static inline unsigned bta_kernel_format(const struct bta_kernel *k, char *out, unsigned cap) {
    /* The names kernel_sigs.h gives these entries, so a pasted report line leads
     * straight back to the signature that did not resolve. */
    static const char *const names[BTA_SIG_ROUTINES] =
        { "copyout", "bt_lock", "bt_unlock", "command", "patch_site", "fp_cache" };
    struct bta_kw w = { out, 0, cap ? cap : 1 };
    bta_kw_s(&w, "kernel: base ");
    if (!k->base) {
        bta_kw_s(&w, "not recovered, fw ");
        bta_kw_x(&w, k->fw_version, 8);
        w.o[w.n] = 0;
        return w.n;
    }
    bta_kw_s(&w, "ok, ");
    unsigned ok = 0, bad = 0;
    for (unsigned i = 0; i < BTA_SIG_ROUTINES; i++) { if (k->sig_flag[i] == 0) ok++; else bad++; }
    bta_kw_c(&w, (char)('0' + ok)); bta_kw_c(&w, '/'); bta_kw_c(&w, (char)('0' + BTA_SIG_ROUTINES));
    bta_kw_s(&w, " signatures");
    if (bad) {
        bta_kw_s(&w, " (");
        unsigned shown = 0;
        for (unsigned i = 0; i < BTA_SIG_ROUTINES; i++) {
            if (k->sig_flag[i] == 0) continue;
            if (shown++) bta_kw_s(&w, ", ");
            bta_kw_s(&w, names[i]);
            /* The accept site stops matching once its own bytes are flipped, so say
             * what that means rather than reporting the routine as missing. */
            if (i == BTA_SIG_PATCH && k->patch_live) bta_kw_s(&w, " already patched");
            else bta_kw_s(&w, k->sig_flag[i] == 2 ? " ambiguous" : " not found");
        }
        bta_kw_c(&w, ')');
    }
    bta_kw_s(&w, ", fw ");
    bta_kw_x(&w, k->fw_version, 8);
    bta_kw_s(&w, ", ");
    unsigned parts = 0;
    if (k->caps & BTA_KCAP_PATCH)   { bta_kw_s(&w, "patch"); parts++; }
    if (k->caps & BTA_KCAP_DEVLIST) { if (parts++) bta_kw_c(&w, '+'); bta_kw_s(&w, "devlist"); }
    if (k->caps & BTA_KCAP_WIDEN)   { if (parts++) bta_kw_c(&w, '+'); bta_kw_s(&w, "widen"); }
    if (!parts) bta_kw_s(&w, "no capabilities");
    else if (parts == 1) bta_kw_s(&w, " only");
    w.o[w.n] = 0;
    return w.n;
}

/* Resolver shell and the service entry points, implemented in app/bt_patch.c. */
int bta_kernel_init(void);
unsigned bta_kernel_caps(void);
const struct bta_kernel *bta_kernel_info(void);
unsigned bta_kernel_report(char *out, unsigned cap);
/* Apply the accept patch. 0 = applied, or already applied (either by this run or by
 * an earlier one in the same jailbreak). Nonzero means it is not in place: no kernel
 * base (1), the site unresolved (2), the write did not take (5), or the live bytes
 * matched neither form (22). A nonzero result is not a reason to stop: the caller
 * logs it and tries the connection anyway. bt_list and bt_prepare, the other two
 * capability-dependent entry points, are declared in app/protocol.h beside the
 * structures they fill. */
int bta_apply_patch(void);

#endif
