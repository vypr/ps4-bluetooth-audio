/* One HCI Change Connection Packet Type (0x040F) experiment for FW14.
 * Mirrors the native FW14 command call at 0x5c10f5, but never modifies
 * link+0x1564 or any other Bluetooth kernel data directly.
 * V7 permits READ-ONLY observation of the explicitly identified SpaceBuds Air.
 * Only the opt-in experimental build allows HCI packet-type recovery there.
 * Native command queue return is not controller command completion.
 */
#include "fw14_slots.h"

#define SLOTS_FW14_FMT_OFF  0xad7822UL  /* FW14 native kernel string "22" */
#define SLOTS_FW14_CMD_OFF  0x5ae660UL /* signature-resolved dispatcher */
#define SLOTS_FW14_OP_OFF   0x5c10fcUL /* native mov esi,0x040f */
#define SLOTS_FW14_MASK_OFF 0x5c1101UL /* native mov r8d,0x3c18 */

__attribute__((noinline)) static long sc3(long n, long a, long b, long c) {
    long out;
    __asm__ volatile("syscall; jnc 1f; neg %%rax; 1:"
        : "=a"(out), "+d"(c), "+D"(a), "+S"(b)
        : "a"(n) : "rcx", "r8", "r9", "r10", "r11", "cc", "memory");
    return out;
}
struct request { u64 mac; int issue; struct bta_slots_sample *out; struct bta_kernel k; };
struct syscall_args { void *fn; struct request *request; };
static int kernel_callback(void *td, struct syscall_args *args) {
    (void)td;
    struct request *in = args->request;
    const struct bta_kernel *k = &in->k;
    struct bta_slots_sample result;
    for (unsigned i=0; i<sizeof(result); i++) ((volatile u8 *)&result)[i] = 0;
    result.status=BTA_SLOTS_UNSUPPORTED; result.command_rc=-1;
    if (k->fw_version != BTA_FW_14008001 ||
        !k->base || !k->ctx_ptr || !k->copyout || !k->lock || !k->unlock ||
        k->command != k->base + SLOTS_FW14_CMD_OFF ||
        k->ctx_ptr != k->base + 0x2612550UL ||
        !bta_slots_observe_allowed(in->mac)) return 22;
    /* Fail closed unless the exact native FW14 packet-type dispatcher is present. */
    const u8 *fmt=(const u8 *)(k->base+SLOTS_FW14_FMT_OFF);
    const u8 *op=(const u8 *)(k->base+SLOTS_FW14_OP_OFF);
    const u8 *mask=(const u8 *)(k->base+SLOTS_FW14_MASK_OFF);
    if (fmt[0]!='2' || fmt[1]!='2' || fmt[2]!=0 ||
        op[0]!=0xbe || op[1]!=0x0f || op[2]!=0x04 || op[3]!=0 || op[4]!=0 ||
        mask[0]!=0x41 || mask[1]!=0xb8 || mask[2]!=0x18 || mask[3]!=0x3c ||
        mask[4]!=0 || mask[5]!=0) return 22;

    int (*lock)(void)=(void *)k->lock;
    int (*unlock)(int)=(void *)k->unlock;
    int rc=lock(); if (rc) return rc;

    u8 *ctx=*(u8 **)k->ctx_ptr;
    result.status=BTA_SLOTS_NO_LINK;
    if (ctx && ((u64)ctx >> 48)==0xffff) {
        for (unsigned i=0;i<4;i++) {
            u8 *slot=ctx+0x23080UL+i*0x4480UL;
            u8 *link=*(u8 **)slot;
            if (!link || ((u64)link>>48)!=0xffff) continue;
            u64 addr=0;
            for (unsigned j=0;j<6;j++) addr|=(u64)link[0x30+j]<<(8*j);
            if (addr!=in->mac) continue;
            result.link_address=(u64)link;
            result.channel=*(u32 *)(link+0xb20);
            result.mode=*(u32 *)(link+0x1c);
            result.maxslots=*(u32 *)(link+0x1560);
            result.pktmask=*(u32 *)(link+0x1564);
            result.pending=*(u32 *)(link+0x12d8);
            result.active=*(u32 *)(link+0x12dc);
            result.handle=*(unsigned short *)(link+0x48);
            result.countdown=*(u32 *)(slot+0x306c);
            result.pcm=*(u32 *)(slot+0x2e40);
            result.submitted=*(u32 *)(slot+0x3934);
            result.status=BTA_SLOTS_OBSERVE;
            if (in->issue) {
                /* Revalidate device allowlist even if caller bypasses userspace. */
                if (!bta_slots_command_target(in->mac)) {
                    result.status=BTA_SLOTS_BAD_TARGET;
                    break;
                }
                u8 *dev=*(u8 **)(link+0x638);
                if (!dev || ((u64)dev>>48)!=0xffff ||
                    !(*(u32 *)(dev+0x20)&8)) {
                    result.status=BTA_SLOTS_BAD_TARGET;
                } else if (result.channel!=10 || !(result.active&8)) {
                    result.status=BTA_SLOTS_WAIT_SERVICE;
                } else if (result.maxslots>=3) {
                    result.status=BTA_SLOTS_ALREADY_WIDE;
                } else if (result.mode!=0) {
                    result.status=BTA_SLOTS_WAIT_ACTIVE;
                } else if (bta_slots_dispatch_allowed(addr,result.mode,result.active,
                               result.channel,result.maxslots,result.handle)) {
                    int (*command)(void *,unsigned,const char *,...)=(void *)k->command;
                    result.command_rc=command(ctx,0x040fu,(const char *)fmt,
                                      (unsigned)result.handle,BTA_FW14_SLOTS_MASK);
                    result.status=result.command_rc<0 ?
                        BTA_SLOTS_COMMAND_ERROR : BTA_SLOTS_SENT;
                } else result.status=BTA_SLOTS_BAD_TARGET;
            }
            break;
        }
    }
    unlock(0);
    int (*copyout)(const void *,void *,u64)=(void *)k->copyout;
    return copyout(&result,in->out,sizeof(result));
}
int bt_fw14_slots_experiment(u64 mac,int issue,struct bta_slots_sample *sample) {
    if (!sample || !bta_slots_observe_allowed(mac) ||
        (issue && !bta_slots_command_target(mac))) return 22;
    if (bta_kernel_init()) return 1;
    const struct bta_kernel *k=bta_kernel_info();
    if (k->fw_version!=BTA_FW_14008001 || !k->command ||
        !k->copyout || !k->ctx_ptr || !k->lock || !k->unlock) return 1;
    struct request in;
    in.mac=mac; in.issue=issue; in.out=sample; in.k=*k;
    return (int)sc3(11,(long)kernel_callback,(long)&in,0);
}
