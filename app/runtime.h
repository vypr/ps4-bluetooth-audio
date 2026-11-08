#ifndef BTA_RUNTIME_H
#define BTA_RUNTIME_H
#include "bta_types.h"
__attribute__((noinline)) static long sc3(long n,long a,long b,long c){long o;__asm__ volatile("syscall; jnc 1f; neg %%rax; 1:":"=a"(o),"+d"(c),"+D"(a),"+S"(b):"a"(n):"rcx","r8","r9","r10","r11","cc","memory");return o;}
__attribute__((noinline)) static long sc5(long n,long a,long b,long c,long d,long e){
 register long r10 __asm__("r10")=d;register long r8 __asm__("r8")=e;long o;
 __asm__ volatile("syscall; jnc 1f; neg %%rax; 1:":"=a"(o),"+d"(c),"+D"(a),"+S"(b),"+r"(r10),"+r"(r8):"a"(n):"rcx","r9","r11","cc","memory");return o;
}
void *memset(void*,int,u64);
void *memcpy(void*,const void*,u64);
static inline u64 length(const char*s){u64 n=0;while(s[n])n++;return n;}
static inline int equal(const char*a,const char*b){while(*a&&*a==*b){a++;b++;}return *a==*b;}
static inline int starts(const char*s,const char*p){while(*p)if(*s++!=*p++)return 0;return 1;}
static inline char*find(char*s,const char*p){for(;*s;s++)if(starts(s,p))return s;return 0;}
static inline void closefd(long fd){if(fd>=0)sc3(6,fd,0,0);}
static inline int exists(const char*p){long f=sc3(5,(long)p,0,0);if(f<0)return 0;closefd(f);return 1;}
static inline long put(const char*p,const void*b,u64 n){long f=sc3(5,(long)p,0x601,0644);if(f<0)return f;long r=sc3(4,f,(long)b,n);closefd(f);return r==(long)n?0:-1;}
static inline long resolve(const char*n,void*p){return sc3(591,0x2001,(long)n,(long)p);}
struct sockaddr4{u8 len,family;u16 port;u32 addr;u8 zero[8];};
static inline int local_ip(const u8*p){return p[0]==127||p[0]==10||(p[0]==192&&p[1]==168)||(p[0]==172&&p[1]>=16&&p[1]<=31);}
static inline int nonblock(long fd){int v=1;return sc3(54,fd,0x8004667e,(long)&v);}
void notify(const char*message);
void* bta_worker(void*);
#define BTA_PORT 8097
/* The control server's listening socket on BTA_PORT (all interfaces, non-blocking),
 * or -1 when the port is taken. The launcher opens it; the worker opens it again
 * after rest mode aborts it. */
static inline long bta_listen(void){
 long fd=sc3(97,2,1,0);if(fd<0)return -1;
 int one=1;sc5(105,fd,0xffff,4,(long)&one,4);
 struct sockaddr4 sa={.len=16,.family=2,.port=(BTA_PORT>>8)|((BTA_PORT&255)<<8)};
 if(sc3(104,fd,(long)&sa,16)<0||sc3(106,fd,4,0)<0||nonblock(fd)<0){closefd(fd);return -1;}
 return fd;
}
/* Control-API schema version. Bumped when the JSON shape or the set of
 * endpoints changes, so a client can tell an old service from a new one
 * independently of the release version. */
#define BTA_API_VERSION 6
#define BTA_STOP "/data/bta_audio/stop"
#define TV_ON "/data/bt_bta_tv_on"
/* Phone control: present => LAN clients may pair; absent (default) => the
 * control API is reachable only from the console itself. The service writes the
 * current six-digit pairing code to PAIR_CODE while this is on (also shown on
 * the TV), so the owner can read it over FTP if needed. */
#define LAN_ON "/data/bta_audio/lan_on"
#define PAIR_CODE "/data/bta_audio/pair_code.txt"
#endif
