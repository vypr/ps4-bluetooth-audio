#ifndef BTA_SIGSCAN_H
#define BTA_SIGSCAN_H
/* Masked byte-pattern search for locating a kernel routine by its own code.
 *
 * A signature is a short run of bytes with a mask: mask byte 0xff means "this byte
 * must match", 0x00 means "ignore it" (used for the operand bytes that legitimately
 * move between kernel builds - RIP-relative displacements, call/jmp rel32, absolute
 * pointers - leaving the opcode/ModRM skeleton). bta_sig_scan reports the single
 * place a signature matches, or that it matches nowhere, or more than once.
 *
 * The matcher is all-or-nothing on purpose: a caller that locates a routine this
 * way must treat anything but one match as "don't know where this is", so an
 * unrecognised or mis-detected routine is never used. See app/kernel.h, which
 * sweeps the kernel text for all six signatures in one pass (bta_kernel_scan)
 * using bta_sig_match below. Pure and freestanding: no libc, no allocation. */

#define BTA_SIG_MAX 48u   /* upper bound on a signature's length (longest is 39) */

struct bta_sig {
    unsigned len;                     /* bytes compared, 1..BTA_SIG_MAX */
    unsigned char bytes[BTA_SIG_MAX]; /* expected bytes; masked-out slots ignored */
    unsigned char mask[BTA_SIG_MAX];  /* 0xff = compare, 0x00 = wildcard */
};

/* Does sig match the bytes at p, given avail bytes are readable from p? The one
 * place the compare lives, so the single-signature scan and the all-six sweep in
 * kernel.h test bytes identically. */
static inline int bta_sig_match(const unsigned char *p, unsigned long avail,
                                const struct bta_sig *sig) {
    if (sig->len == 0 || sig->len > avail) return 0;
    for (unsigned j = 0; j < sig->len; j++) {
        unsigned char m = sig->mask[j];
        if ((unsigned char)(p[j] & m) != (unsigned char)(sig->bytes[j] & m)) return 0;
    }
    return 1;
}

/* Search [buf, buf+len) for sig. Returns the single match offset, or -1 for no
 * match, or -2 for more than one match (ambiguous: the caller must refuse). */
static inline long bta_sig_scan(const unsigned char *buf, unsigned long len,
                                const struct bta_sig *sig) {
    long hit = -1;
    if (sig->len == 0 || sig->len > len) return -1;
    for (unsigned long i = 0, n = len - sig->len; i <= n; i++) {
        if (bta_sig_match(buf + i, len - i, sig)) {
            if (hit >= 0) return -2;   /* a second match: ambiguous */
            hit = (long)i;
        }
    }
    return hit;
}

#endif
