/* Read the driver's Classic device cache and request the normal three-slot HCI
 * packet types for a link, exactly as the proven BTA bridge did. Every kernel
 * address and record-layout offset comes from the runtime resolution in
 * app/kernel.h (bt_list/bt_prepare resolve it on first use); this file holds only
 * the logic that walks the kernel structures. Both features need the device cache
 * and the per-link records, which are located from the known-firmware table, so on
 * any firmware but 13.520.001 their capabilities stay clear and these refuse
 * cleanly - the service carries on with what it does have. */
#include "protocol.h"
#include "kernel.h"
__attribute__((noinline)) static long sc3(long n, long a, long b, long c) {
    long o;
    __asm__ volatile("syscall; jnc 1f; neg %%rax; 1:" : "=a"(o), "+d"(c), "+D"(a), "+S"(b) : "a"(n) : "rcx", "r8", "r9", "r10", "r11", "cc", "memory");
    return o;
}

/* Everything the ring-0 callback needs, copied off the cached resolution in
 * userland: a ring-0 callback must read only the syscall-arg pointer, never a
 * payload global. */
struct input { u64 address; int mode; struct bt_devices *out; struct bta_kernel k; };
struct args { void *fn; struct input *input; };

/* Mode 0 copies the whole device cache out; mode 1 asks the link for one device to
 * agree the three-slot packet types. Runs on the calling worker's private stack via
 * syscall 11, like the other kernel helpers here. The addresses are already
 * signature-resolved, so this walks straight into them. */
static int callback(void *td, struct args *a) {
    (void)td;
    struct input *in = a->input;
    const struct bta_kernel *k = &in->k;
    u64 address = in->address;
    int mode = in->mode;
    if (mode != 0 && mode != 1) return 22;
    if (mode == 1 && (!address || address >> 48)) return 22;

    struct bt_devices out;
    for (unsigned i = 0; i < sizeof(out); i++) ((volatile unsigned char *)&out)[i] = 0;

    int (*lock)(void) = (void *)k->lock;
    int (*unlock)(int) = (void *)k->unlock;
    int rc = lock();
    if (rc) return rc;
    unsigned char *ctx = *(void **)k->ctx_ptr;
    if (!ctx || ((u64)ctx >> 48) != 0xffff) { unlock(0); return 6; }

    if (mode == 0) {
        for (unsigned off = k->cache_first; off < k->cache_end; off += k->cache_stride) {
            unsigned char *d = ctx + off;
            u64 mac = *(u64 *)d;
            if (!mac || mac >> 48) continue;
            struct bt_device *v = &out.items[out.count++];
            v->address = mac;
            v->cod = *(unsigned *)(d + k->dev_cod);
            v->services = *(unsigned *)(d + k->dev_services);
            v->flags = *(unsigned *)(d + k->dev_flags);
            for (unsigned i = 0; i < sizeof(v->name) - 1; i++) {
                unsigned char c = d[k->dev_name + i];
                if (!c) break;
                v->name[i] = (c >= 32 && c < 127) ? c : '?';
            }
            if (out.count == BT_DEVICE_LIMIT) break;
        }
    } else {
        rc = 2;
        for (unsigned off = k->link_first; off < k->link_end; off += k->link_stride) {
            unsigned char *c = ctx + off;
            if (*(u64 *)(c + k->link_address) != address) continue;
            unsigned char *d = *(void **)(c + k->link_info);
            if (!d || ((u64)d >> 48) != 0xffff || !(*(unsigned *)(d + k->dev_services) & 8)) { rc = 93; break; }
            if (*(unsigned *)(c + k->link_pkt_count) >= 3) { rc = 0; break; }
            int (*command)(void *, unsigned, const char *, ...) = (void *)k->command;
            rc = command(ctx, k->cmd_set_pkt_type, (const char *)k->fmt_string,
                         (unsigned)*(unsigned short *)(c + k->link_handle), k->pkt_types_3slot);
            if (rc >= 0) { *(unsigned *)(c + k->link_pkt_types) = k->pkt_types_3slot; rc = 0; }
            break;
        }
    }
    unlock(0);
    if (mode == 0) {
        int (*copyout)(const void *, void *, u64) = (void *)k->copyout;
        return copyout(&out, in->out, sizeof(out));
    }
    return rc;
}

/* Fill the arg from the cached resolution (userland) and enter ring 0. Returns
 * nonzero when the required capability is absent, so the worker degrades rather
 * than refusing the whole service. */
static int invoke(struct input *in, unsigned need) {
    if (bta_kernel_init()) return 1;
    const struct bta_kernel *k = bta_kernel_info();
    if (!(k->caps & need)) return 1;
    in->k = *k;
    return (int)sc3(11, (long)callback, (long)in, 0);
}

int bt_list(struct bt_devices *out) {
    struct input in; in.address = 0; in.mode = 0; in.out = out;
    return invoke(&in, BTA_KCAP_DEVLIST);
}
int bt_prepare(u64 address) {
    struct input in; in.address = address; in.mode = 1; in.out = 0;
    return invoke(&in, BTA_KCAP_WIDEN);
}
