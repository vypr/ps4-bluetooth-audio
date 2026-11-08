/* Kernel resolver and the Bluetooth service-acceptance patch.
 *
 * At first use this resolves the kernel routines by signature (one ring-0 pass over
 * the kernel text), caches the result in a userland global, and derives every
 * capability from what resolved - no firmware version gate anywhere. The patch then
 * flips one in-RAM instruction so the stock allowlist also admits audio-class
 * devices. It changes no flash or firmware files and clears on reboot.
 *
 * The pure resolution/report logic lives in app/kernel.h so the host tests can
 * exercise it without a PS4 toolchain; this file is only the ring-0/syscall shell.
 * Ring-0 callbacks must not read a payload global (not relocated in that context),
 * so each copies everything it needs onto the caller's stack and passes it through
 * the single syscall-arg pointer, exactly as the proven probe payload did. */
#include "kernel.h"
#include "kernel_sigs.h"

__attribute__((noinline)) static long sc3(long n, long a, long b, long c) {
    long out;
    __asm__ volatile("syscall; jnc 1f; neg %%rax; 1:" : "=a"(out), "+d"(c), "+D"(a), "+S"(b) : "a"(n) : "rcx", "r8", "r9", "r10", "r11", "cc", "memory");
    return out;
}

/* The one cached resolution. Userland only; the ring-0 passes fill it via copyout. */
static struct bta_kernel g_kernel;
static int g_kernel_ready;

#define BTA_KSCAN 0x600000UL   /* kernel-text window to sweep; proven on 13.52 hardware */

/* --- the resolve pass (ring 0) -------------------------------------------------- */
/* GoldHEN's syscall 11 calls fn(td, uap); uap overlays the syscall args, so the
 * second parameter is the {fn, arg} pair and ->arg is the pointer we passed. */
struct kinit_arg {
    struct bta_kernel *out;             /* userland destination for copyout */
    struct bta_sig sigs[BTA_SIG_COUNT]; /* copied off the global onto our stack */
    unsigned nsig, fw_version;
    u64 scan;
};
struct kinit_args { void *fn; struct kinit_arg *arg; };

static int kresolve(void *td, struct kinit_args *a) {
    (void)td;
    struct kinit_arg *arg = a->arg;
    unsigned lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xc0000082u)); /* LSTAR */
    u64 lstar = ((u64)hi << 32) | lo;
    /* The syscall entry stub sits a fixed delta below the kernel text base, and that
     * delta is smaller than the 16 KiB text alignment, so masking the low bits
     * recovers the base with no per-firmware constant. A nonsense base resolves
     * nothing. */
    u64 base = lstar & ~0x3fffUL;
    if ((base >> 48) != 0xffff) return 1;

    struct bta_kernel k;
    for (unsigned i = 0; i < sizeof(k); i++) ((volatile u8 *)&k)[i] = 0;
    k.base = base;
    k.fw_version = arg->fw_version;
    bta_kernel_resolve(&k, (const unsigned char *)base, arg->scan, arg->sigs, arg->nsig);

    /* copyout is itself one of the six: without it there is no channel back to
     * userland, so report nothing and let every capability stay unavailable. */
    if (!k.copyout) return 2;
    int (*copyout)(const void *, void *, u64) = (void *)k.copyout;
    return copyout(&k, arg->out, sizeof(k));
}

int bta_kernel_init(void) {
    if (g_kernel_ready) return g_kernel.base ? 0 : 1;
    g_kernel_ready = 1; /* resolve once; a failed pass is not retried every call */

    /* Firmware version in userland (a ring-0 callback must not call libkernel). */
    struct { u64 size; char text[28]; unsigned version; } v = {.size = 40};
    int (*getver)(void *) = 0;
    unsigned fw = 0;
    if (!sc3(591, 0x2001, (long)"sceKernelGetSystemSwVersion", (long)&getver) && getver && !getver(&v))
        fw = v.version;

    /* Copy the signatures onto our stack (volatile byte loops, so -O2 synthesises no
     * memcpy) and pass them by pointer; the ring-0 pass reads them from the arg. */
    struct kinit_arg arg;
    for (unsigned i = 0; i < sizeof(arg); i++) ((volatile u8 *)&arg)[i] = 0;
    for (unsigned s = 0; s < BTA_SIG_COUNT; s++) {
        const volatile u8 *src = (const volatile u8 *)&bta_kernel_sigs[s].sig;
        volatile u8 *dst = (volatile u8 *)&arg.sigs[s];
        for (unsigned b = 0; b < sizeof(struct bta_sig); b++) dst[b] = src[b];
    }
    arg.out = &g_kernel;
    arg.nsig = BTA_SIG_COUNT;
    arg.fw_version = fw;
    arg.scan = BTA_KSCAN;

    /* Pre-zero the global so a failed pass (no base, or copyout unresolved) leaves
     * every field and capability clear. */
    for (unsigned i = 0; i < sizeof(g_kernel); i++) ((volatile u8 *)&g_kernel)[i] = 0;
    sc3(11, (long)kresolve, (long)&arg, 0);
    g_kernel.fw_version = fw; /* keep it reportable even when the pass copied nothing out */
    return g_kernel.base ? 0 : 1;
}

unsigned bta_kernel_caps(void) { bta_kernel_init(); return g_kernel.caps; }
const struct bta_kernel *bta_kernel_info(void) { bta_kernel_init(); return &g_kernel; }
unsigned bta_kernel_report(char *out, unsigned cap) {
    bta_kernel_init();
    return bta_kernel_format(&g_kernel, out, cap);
}

/* --- the accept patch (ring 0) -------------------------------------------------- */
/* Only the site address crosses into ring 0, and the verdict comes back through the
 * syscall return value: nothing is written to user memory (copyout is used for that
 * elsewhere), so the callback never touches the caller's pages. */
struct kpatch_arg { u64 site; };
struct kpatch_args { void *fn; struct kpatch_arg *arg; };

static int kpatch(void *td, struct kpatch_args *a) {
    (void)td;
    volatile u64 *site = (volatile u64 *)a->arg->site;
    u64 original = BTA_ACCEPT_ORIGINAL, replacement = BTA_ACCEPT_REPLACEMENT;
    u64 before = *site;
    if (before == original) {
        /* How you write to kernel text without corrupting it: mask interrupts, clear
         * CR0.WP, do the swap atomically, restore CR0 and flags, read back. Not a
         * gate - it costs nothing and keeps the write safe. */
        u64 flags, cr0, expected = original;
        __asm__ volatile("pushfq; popq %0; cli; mov %%cr0,%1" : "=r"(flags), "=r"(cr0) :: "memory");
        u64 writable = cr0 & ~0x10000UL;
        __asm__ volatile("mov %0,%%cr0" :: "r"(writable) : "memory");
        __asm__ volatile("lock cmpxchgq %2,(%1)" : "+a"(expected) : "r"(site), "r"(replacement) : "cc", "memory");
        __asm__ volatile("mov %0,%%cr0; pushq %1; popfq" :: "r"(cr0), "r"(flags) : "cc", "memory");
        if (expected != original || *site != replacement) return 5;
        return 0;
    }
    if (before == replacement) return 0; /* already patched */
    return 22;                           /* neither form: do not write */
}

/* 0 = applied or already applied. Nonzero means the accept patch is not in place:
 * no kernel base (1), patch site unresolved (2), the write did not take (5), or the
 * live bytes matched neither the original nor the patched form (22).
 *
 * patch_live means this service already patched the site in an earlier run of the
 * same jailbreak; kpatch then finds the replacement and reports success without
 * writing. Either address is the same instruction, so pass whichever resolved. */
int bta_apply_patch(void) {
    if (bta_kernel_init()) return 1;
    u64 site = g_kernel.patch_site ? g_kernel.patch_site : g_kernel.patch_live;
    if (!site) return 2;
    struct kpatch_arg arg;
    for (unsigned i = 0; i < sizeof(arg); i++) ((volatile u8 *)&arg)[i] = 0;
    arg.site = site;
    return (int)sc3(11, (long)kpatch, (long)&arg, 0);
}
