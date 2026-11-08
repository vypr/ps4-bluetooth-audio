/* Bluetooth Audio service — orchestration.
 *
 * This file wires the pieces together and owns everything that must talk to the
 * PS4: the Bluetooth and HDMI drivers, the audio capture ring, the local HTTP
 * control server and the real-time sender thread. The logic that does not need
 * the console lives in small, unit-tested headers beside it:
 *
 *   json.h     bounded JSON writer for the status and timing responses
 *   strfmt.h   message/MAC/path string helpers
 *   dsp.h      float capture -> 16-bit PCM, the test tone and the ring maths
 *   conn.h     the connection state machine: link state, backoff and SBC mode
 *   profiles.h remembered-device table and selection policy
 *   pacing.h   clock-paced sending with a bounded latency buffer
 *   timing.h   per-second send statistics (GET /api/timing)
 *   auth.h     LAN pairing and token checks for phone control
 *
 * The driver helpers sit behind kernel.h, which resolves the kernel routines by
 * signature at startup and says which it found (bta_kernel_caps). So no firmware
 * offset appears here, and nothing here refuses to try: a capability the kernel
 * didn't yield costs a feature, never the attempt. Any firmware, any model. */
#include "runtime.h"
#include "web_page.h"
#include "protocol.h"
#include "json.h"
#include "auth.h"
#include "strfmt.h"
#include "dsp.h"
#include "conn.h"
#include "profiles.h"
#include "timing.h"
#include "diag_v6.h"
#include "v8_diag.h"
#include "v8_gain.h"
#include "v11_volume.h"
#include "v9_tune.h"
#include "v10_recovery.h"
#include "pacing.h"
#include "v12_tone.h"
#include "v13_sender.h"
#include "v14_hdmi.h"
#include "kernel.h"
#include "fw14_slots.h"

/* FreeBSD syscall numbers used directly through sc3/sc5. */
#define SYS_read    3
#define SYS_write   4
#define SYS_open    5
#define SYS_unlink  10
#define SYS_chmod   15
#define SYS_accept  30
#define SYS_getsockname 32
#define SYS_ioctl   54
#define SYS_socket  97
#define SYS_connect 98
#define SYS_setsockopt 105
#define SYS_gettimeofday 116
#define SYS_rename  128
#define SYS_mkdir   136

/* Bluetooth driver ioctls on /dev/bt. */
#define BT_SCAN_START  0x20046204
#define BT_SCAN_STOP   0x20046205
#define BT_LINK_STATE  0x80086203
#define BT_CONNECT     0x8008620a
#define BT_DROP_LINK   0x8008620b
#define BT_PAIR_ACCEPT 0x80106208
#define BT_PIN_REPLY   0x80186207
#define BT_SEND_AUDIO  0x80206216
/* HDMI driver ioctl on /dev/hdmi: set the mute flag. */
#define HDMI_SET_MUTE  0xc0048d05

/* Keep HDMI unmuted for this long after streaming starts so the system audio
 * mix that feeds /shm_acap.full actually activates, THEN apply the user's
 * mute preference. Starting muted leaves the capture ring empty (silence). */
#define PRIME_US 2000000
/* Automatic recovery threshold: the PS4 refused every packet for this long. */
#define REFUSED_US 4000000
/* After BTA_MAX_PAIR_FAILS wrong pairing codes, reject further attempts for this
 * long. Rate-limits a LAN brute force without a permanent lockout. */
#define PAIR_COOLDOWN_US 30000000

struct audio_req { u64 address; void *samples; u64 length; u32 mode, pad; };
/* A response is `total` bytes: the first `head` come from output (the header, and
 * the body when it fits), the rest straight from `tail`, a constant body such as
 * the page, which is larger than output. */
struct client { long fd; u64 deadline; u32 used, sent, head, total; const char *tail; int loopback; char input[2048]; char output[20000]; };
struct app {
    int (*sleep)(unsigned);
    u64 (*clock)(void);
    int (*shm)(const char *, int, int);
    void *(*map)(void *, u64, int, int, int, u64);
    int (*unmap)(void *, u64);
    long listener, bt, hdmi, shmfd;
    u8 *ring;
    u32 readpos;
    int wanted, streaming, tv, muted, quit;
    int patch_tried;     /* the accept patch is idempotent: applied once per run */
    enum conn_state state;
    long patch_rc, packet_rc, connect_rc, last_rc, hdmi_rc, btstate;
    u64 next_try, deadline, heartbeat, poll_at, net_at, packets, errors, resyncs;
    unsigned level;
    int volume;
    char message[128];
    /* bta_kernel_report's line, logged at startup and served in the status. 160 to fit
     * the worst case (several routines named) without clipping: it is what a user on an
     * untested firmware quotes in a bug report. The log carries it in full. */
    char kernel[160];
    /* Per-second send timing (see timing.h). */
    struct bta_second secs[128], cur;
    u32 sec_seq;
    u64 sec_start, last_send;
    /* V6: read-only diagnostic counters; no extra driver polls. */
    struct bta_diag6_window diag6;
    struct bta_v8_diag v8;
    struct bta_v8_gain v8_gain;
    struct bta_v9_tune tune;
    u32 v9_last_trim_ms, v9_last_late8, v9_last_maxlate_us, v9_last_calls8;
    /* V10 manual recovery; intentionally never automatic. */
    u64 v10_reset_next_at, v10_reset_at;
    u32 v10_reset_count, v10_reset_kind;
    u32 v10_reset_pending, v10_reset_stream_from;
    u64 diag6_at;
    u32 diag6_stream_id;
    int diag6_test_prev;
    int mode;            /* current SBC send mode: SBC_JOINT / SBC_DUAL */
    u8 modes_tried;      /* modes attempted this stream (for one-shot fallback) */
    int confirmed;       /* a packet was accepted this stream (record the mode once) */
    /* Clock-paced sending (see pacing.h). latency_mode picks the backlog target;
     * drops/underruns count trims and starved slots for the diagnostics. */
    struct bta_pace pace;
    int latency_mode;
    u64 drops, underruns;
    u64 refused_since;
    /* FW14 V5: per-link cap, 5s cross-link and 20s same-link cooldowns. */
    u64 slots_exp_at;
    struct bta_slots_recovery_budget slots_exp_budget;
    u64 slots_exp_last_link;
    unsigned slots_exp_narrow_samples, slots_exp_wide_samples;
    int slots_exp_armed, slots_exp_disabled, slots_exp_have_previous;
    u32 slots_exp_previous_mode, slots_exp_previous_slots;
    /* V7: bounded read-only FW14 link snapshots, throttled to 5s or change. */
    u64 v7_observe_at, v7_observed_mac;
    struct bta_slots_sample v7_last;
    int v7_have_last;
    u32 relinks, relinks_total;
    u64 stream_at, test_until;
    u32 test_phase;
    struct bta_v12_tone tone_clock;
    struct bta_v13_sender sender_diag;
    struct bta_v14_hdmi hdmi_diag;
    int snap; /* on the next capture read, drop to the live write cursor */
    void *mutex;
    int (*mlock)(void **);
    int (*munlock)(void **);
    volatile int sender_run, sender_alive;
    int sender_prio;
    long thread_rc, prio_rc, aff_rc, pi_rc;
    /* Truthful link state (see conn.h / link_state). dropped: the link was lost
     * while streaming. give_up: the backoff schedule ran out, so we wait for the
     * user. announced: a "disconnected" notice was shown. session: this
     * connection was announced. */
    int dropped, give_up, announced, session, meter;
    u32 attempts;        /* consecutive failed attempts (drives the backoff) */
    u64 ok_at, meter_at, notify_at;
    unsigned meter_peak;
    struct bt_device target;
    u64 own_link;        /* address of the link this run dialled; 0 once dropped (see conn_plan_link) */
    u64 redial_at;       /* a stale link was just dropped: dial afresh at this time */
    struct bta_profiles profiles;
    int profile_dirty;   /* profiles changed in memory, persist on the next beat */
    u64 last_tick;       /* worker-loop time, for rest-mode resume detection */
    u64 last_wall;       /* wall-clock time of that tick (see conn_slept) */
    u64 listen_at;       /* earliest next re-open of the listening socket */
    u32 accept_errs;     /* accept failures in a row (a broken listener) */
    struct bt_devices devices;
    int scanning, pin_pending;
    u64 scan_end, list_at;
    /* Searching and dialling never overlap (conn.h "Sharing the radio").
     * page_at: when the outstanding dial was sent (0: none), page_end: when the
     * driver is surely done with it; page_before: the link state read just before
     * it, page_last: the last one logged, page_seen: a reading showed the attempt.
     * hold: what the radio waits for (hold_addr's dial or link, since hold_at);
     * radio_quiet_at: no search or dial starts before this. */
    u64 page_at, page_end;
    long page_before, page_last;
    int page_seen;
    enum conn_hold hold;
    u64 hold_at, hold_addr, hold_poll, radio_quiet_at;
    /* Add Device (conn.h): who opened it; resume: reconnect the device when it
     * ends; scan_wait: a search is due once the radio is free; console_at: the
     * console app's latest request; add_idle: since when nothing was searched. */
    enum conn_add adding;
    int resume, scan_wait;
    u64 console_at, add_idle;
    u64 event_at;        /* driver events logged since then (log_event) */
    u32 event_lines, stream_events;
    /* Devices that answered a search since Add Device opened, with their latest
     * signal strength in dBm (note_found). The driver's device list also holds
     * every device it ever knew, so this is what tells who is there now. */
    struct { u64 address; int rssi; } found[BT_DEVICE_LIMIT];
    u32 found_count;
    struct client clients[4];
    /* LAN access control (see auth.h). The console reaches the service over the
     * loopback address and always has full access; LAN clients must pair. */
    struct bta_auth auth;
    int (*getrandom)(void *, u64);
    /* A lan-on/off request sets these under the lock; network_tick runs the
     * resulting file write and TV notice after releasing it, so the real-time
     * sender is never blocked on flash I/O while toggling phone control. */
    int lan_act;
    char lan_code[BTA_CODE_DIGITS + 1];
    /* The console's LAN address while phone control is on (0 if unknown), shown
     * with the code so a phone knows where to go. Worker thread only. */
    u32 ip;
    u64 ip_at;
    /* Service log (see log_text): lines wait here under the lock until the
     * heartbeat appends them to SERVICE_LOG outside it. */
    char log[1024];
    u32 log_len;
    u64 log_t0, log_size;
    u64 stats_at, stream_packets, stream_resyncs; /* audio summary every 10 min (log_stats) */
    u64 http_at;         /* failed-request log lines bounded since then (log_request) */
    u32 http_lines;
    u64 status_at;       /* next status.json write (every 10 s) */
};

/* Fill buf with n random bytes: the kernel RNG, or — should it not resolve — a
 * mix of clock reads, which is all the console offers without it. */
static void fill_random(struct app *a, u8 *buf, unsigned n) {
    if (a->getrandom && n <= 64 && a->getrandom(buf, n) == 0) return;
    u64 x = a->clock() ^ (u64)(unsigned long)buf;
    for (unsigned i = 0; i < n; i++) { x = (x ^ a->clock()) * 0x100000001b3UL; buf[i] = (u8)(x >> 32); }
}
/* Reconcile the in-memory auth with the LAN_ON flag `on`. Memory only: on a
 * transition it reports what the caller must persist, so the file write (and the
 * TV notification) happen outside the state lock. Returns 1 when phone control
 * just turned on (new code copied to `code`), -1 when it just turned off, else 0. */
static int sync_lan(struct app *a, int on, char code[BTA_CODE_DIGITS + 1]) {
    code[0] = 0;
    if (on && !a->auth.enabled) {
        u8 r[4];
        fill_random(a, r, sizeof(r));
        bta_auth_enable(&a->auth, r);
        for (int i = 0; i <= BTA_CODE_DIGITS; i++) code[i] = a->auth.code[i];
        return 1;
    }
    if (!on && a->auth.enabled) { bta_auth_disable(&a->auth); return -1; }
    return 0;
}
/* The console's LAN address: the source address the network stack picks for an
 * outside destination (a UDP connect sends nothing). 0 without a network, or
 * when it isn't a private LAN address, the only kind phones are accepted from
 * (see network_tick). */
static u32 lan_address(void) {
    long fd = sc3(SYS_socket, 2, 2, 0); /* AF_INET, SOCK_DGRAM */
    if (fd < 0) return 0;
    struct sockaddr4 to = {16, 2, 0x3500, 0x08080808, {0}}; /* 8.8.8.8, port 53 */
    struct sockaddr4 me;
    u32 len = sizeof(me), ip = 0;
    memset(&me, 0, sizeof(me));
    if (sc3(SYS_connect, fd, (long)&to, sizeof(to)) >= 0 && sc3(SYS_getsockname, fd, (long)&me, (long)&len) >= 0) ip = me.addr;
    closefd(fd);
    const u8 *b = (const u8 *)&ip;
    return local_ip(b) && b[0] != 127 ? ip : 0;
}
/* Act on what sync_lan reported, with no lock held: write or remove the code
 * file, look up the console's address and announce both on the TV. */
static void persist_lan(struct app *a, int act, const char *code) {
    if (act == 1) {
        put(PAIR_CODE, code, BTA_CODE_DIGITS);
        a->ip = lan_address();
        a->ip_at = a->clock() + 30000000;
        char m[160];
        if (a->ip) {
            char ip[16];
            struct writer w = {m, 0, sizeof(m)};
            ip_text(a->ip, ip);
            append(&w, "Phone control is on. On your phone, open http://");
            append(&w, ip);
            append(&w, ":8097 and enter code ");
            append(&w, code);
            append(&w, ".");
        } else compose(m, sizeof(m), "Phone control is on. Pairing code: ", code, ". Enter it in your phone's browser.");
        notify(m);
    } else if (act == -1) {
        sc3(SYS_unlink, (long)PAIR_CODE, 0, 0);
    }
}

static void lock_state(struct app *a) { if (a->mlock) a->mlock(&a->mutex); }
static void unlock_state(struct app *a) { if (a->munlock) a->munlock(&a->mutex); }

/* Wall-clock microseconds (0 if unavailable): unlike the process clock (a->clock),
 * it runs on while the console is in rest mode. */
static u64 wall_us(void) {
    struct { long sec, usec; } tv = {0, 0};
    if (sc3(SYS_gettimeofday, (long)&tv, 0, 0) < 0 || tv.sec <= 0) return 0;
    return (u64)tv.sec * 1000000 + (u64)tv.usec;
}

/* Service log: every new status message, connection steps, failed requests and an
 * audio summary, each stamped with the UTC time of day, so what happened on the
 * console can be read back over FTP and lined up with the app's log and KLog.
 * Each start, and each SERVICE_LOG_CAP bytes, moves the log to SERVICE_LOG_OLD:
 * at most two files, and the newest lines are always kept. */
#define SERVICE_LOG "/data/bta_audio/service.log"
#define SERVICE_LOG_OLD "/data/bta_audio/service-previous.log"
#define SERVICE_LOG_CAP 262144
static void log_put(struct app *a, const char *s) {
    for (; *s && a->log_len + 2 < sizeof(a->log); s++) a->log[a->log_len++] = *s;
}
static void log_text(struct app *a, const char *s) {
    char t[24];
    u64 w = wall_us();
    if (w) { /* "[13:44:02] " */
        t[0] = '[';
        clock_text(w / 1000000, t + 1);
        t[9] = ']'; t[10] = ' '; t[11] = 0;
        log_put(a, t);
    } else { /* no wall clock: "[+123.4] ", seconds since the service started */
        u64 ds = (a->clock() - a->log_t0) / 100000; /* tenths of a second */
        unsigned i = sizeof(t) - 1;
        t[i] = 0;
        t[--i] = ' '; t[--i] = ']'; t[--i] = (char)('0' + ds % 10); t[--i] = '.';
        ds /= 10;
        do { t[--i] = (char)('0' + ds % 10); ds /= 10; } while (ds && i > 2);
        t[--i] = '+'; t[--i] = '[';
        log_put(a, t + i);
    }
    log_put(a, s);
    if (a->log_len + 1 < sizeof(a->log)) a->log[a->log_len++] = '\n';
}
static void log_num(struct app *a, const char *label, long v) {
    char b[96];
    struct writer w = {b, 0, sizeof(b)};
    append(&w, label);
    number(&w, v);
    log_text(a, b);
}

/* DIAG14: logging only. No extra Bluetooth calls; no kernel changes. */
static void diag14_hex32(struct writer *w, u32 value) {
    const char digits[] = "0123456789ABCDEF";
    char hex[11];
    hex[0] = '0'; hex[1] = 'x';
    for (unsigned i = 0; i < 8; i++) hex[2 + i] = digits[(value >> (28 - i * 4)) & 15];
    hex[10] = 0;
    append(w, hex);
}
static void diag14_device(struct app *a, const char *stage, const struct bt_device *d,
                          int rssi, int with_rssi, unsigned event_type) {
    char buf[192], mac[13];
    struct writer w = {buf, 0, sizeof(buf)};
    mac_text(d->address, mac);
    append(&w, "DIAG14 "); append(&w, stage);
    append(&w, " mac="); append(&w, mac);
    append(&w, " cod="); diag14_hex32(&w, d->cod);
    append(&w, " services="); diag14_hex32(&w, d->services);
    append(&w, " flags="); diag14_hex32(&w, d->flags);
    append(&w, " audio_hint="); number(&w, bt_is_audio(d));
    if (with_rssi) { append(&w, " rssi="); number(&w, rssi); }
    if (event_type) { append(&w, " event="); number(&w, event_type); }
    log_text(a, buf);
}
static void diag14_dial(struct app *a, u64 address, long rc) {
    char buf[160], mac[13];
    struct writer w = {buf, 0, sizeof(buf)};
    mac_text(address, mac);
    append(&w, "DIAG14 dial mac="); append(&w, mac);
    append(&w, " rc_signed="); number(&w, rc);
    append(&w, " rc32="); diag14_hex32(&w, (u32)rc);
    append(&w, " pre_link_state="); number(&w, a->btstate);
    append(&w, " attempt="); number(&w, (long)a->attempts);
    log_text(a, buf);
}
/* END DIAG14 */
/* "<pre><milliseconds> ms<post>" */
static void log_ms(struct app *a, const char *pre, u64 us, const char *post) {
    char b[96];
    struct writer w = {b, 0, sizeof(b)};
    append(&w, pre);
    number(&w, (long)(us / 1000));
    append(&w, " ms");
    append(&w, post);
    log_text(a, b);
}
/* This stream so far: minutes, packets, and what the pacer had to fix. */
static void log_stats(struct app *a, const char *label) {
    char b[128];
    struct writer w = {b, 0, sizeof(b)};
    append(&w, label);
    number(&w, (long)((a->clock() - a->stream_at) / 60000000));
    append(&w, " min, ");
    number(&w, (long)(a->packets - a->stream_packets));
    append(&w, " packets, ");
    number(&w, (long)a->drops);
    append(&w, " drops, ");
    number(&w, (long)a->underruns);
    append(&w, " underruns, ");
    number(&w, (long)(a->resyncs - a->stream_resyncs));
    append(&w, " resyncs");
    log_text(a, b);
}
/* FW14 V6: emit diagnostic snapshots only from the normal worker heartbeat.
 * The real-time sender increments in-RAM counters after the EXISTING ioctl;
 * this function adds no Bluetooth ioctls, map/remap, flash I/O, or playback changes.
 * "accepted" only means an ioctl returned positive, NOT audible playback. */
static void diag6_emit(struct app *a) {
    const struct bta_diag6_window *d = &a->diag6;
    char b[240];
    struct writer w = {b, 0, sizeof(b)};
    append(&w, "FW14 DIAG6 SEND id="); number(&w, a->diag6_stream_id);
    append(&w, " game="); number(&w, d->game_packets);
    append(&w, " tone="); number(&w, d->tone_packets);
    append(&w, " game_nonzero="); number(&w, d->game_nonzero);
    append(&w, " tone_nonzero="); number(&w, d->tone_nonzero);
    append(&w, " rc1="); number(&w, d->rc_one);
    append(&w, " rc768="); number(&w, d->rc_bytes);
    append(&w, " rc_other="); number(&w, d->rc_other);
    append(&w, " refused="); number(&w, d->refused);
    append(&w, " failed="); number(&w, d->failed);
    append(&w, " last_rc="); number(&w, d->last_rc);
    append(&w, " maxcall_us="); number(&w, d->max_send_us);
    append(&w, " maxgap_us="); number(&w, d->max_gap_us);
    append(&w, " peak_game="); number(&w, d->game_peak);
    append(&w, " peak_tone="); number(&w, d->tone_peak);
    log_text(a, b);
    bta_diag6_reset(&a->diag6);

    w = (struct writer){b, 0, sizeof(b)};
    append(&w, "FW14 DIAG6 CAP id="); number(&w, a->diag6_stream_id);
    append(&w, " streaming="); number(&w, a->streaming);
    append(&w, " shmfd="); number(&w, a->shmfd);
    if (a->ring) {
        u32 header = *(volatile u32 *)a->ring;
        u32 cursor = *(volatile u32 *)(a->ring + 12);
        append(&w, " header="); number(&w, header);
        append(&w, " writepos="); number(&w, cursor);
        append(&w, " readpos="); number(&w, a->readpos);
        if (header == 0x10000 && cursor >= 16 && cursor < 0x10000 && !(cursor & 7) &&
            a->readpos >= 16 && a->readpos < 0x10000 && !(a->readpos & 7)) {
            append(&w, " backlog_bytes="); number(&w, bta_ring_available(cursor, a->readpos));
        } else append(&w, " invalid_ring=1");
    } else append(&w, " ring=unmapped");
    append(&w, " mode="); number(&w, conn_send_type(a->mode));
    append(&w, " muted="); number(&w, a->muted);
    append(&w, " hdmi_rc="); number(&w, a->hdmi_rc);
    append(&w, " drops="); number(&w, a->drops);
    append(&w, " resyncs="); number(&w, a->resyncs);
    log_text(a, b);
}
/* V8: five-second quantitative pacing, trim and limiter summary. */
/* V12: summarize the just-finished 2s tone test, making sender stalls,
 * skipped deadlines and driver refusals distinguishable in service.log. */
static void v12_tone_emit(struct app *a) {
    const struct bta_v12_tone *t = &a->tone_clock;
    char b[320];
    struct writer w = {b, 0, sizeof(b)};
    append(&w, "FW14 V12 TONE packets="); number(&w, t->packets);
    append(&w, " missed_slots="); number(&w, t->skipped);
    append(&w, " late4="); number(&w, t->late4);
    append(&w, " late8="); number(&w, t->late8);
    append(&w, " maxlate_us="); number(&w, t->maxlate_us);
    append(&w, " gaps8="); number(&w, t->gaps8);
    append(&w, " maxgap_us="); number(&w, t->maxgap_us);
    append(&w, " ioctl4="); number(&w, t->ioctl4);
    append(&w, " ioctl8="); number(&w, t->ioctl8);
    append(&w, " maxcall_us="); number(&w, t->maxcall_us);
    append(&w, " refused="); number(&w, t->refused);
    append(&w, " failed="); number(&w, t->failed);
    append(&w, " poll_us="); number(&w, a->tune.poll_us);
    log_text(a, b);
}
/* V13: distinguish slow wake-ups from delays waiting for the shared lock. */
static void v13_sender_emit(struct app *a) {
    const struct bta_v13_sender *d = &a->sender_diag;
    char b[250];
    struct writer w = {b, 0, sizeof(b)};
    append(&w, "FW14 V13 SENDER loops="); number(&w, d->loops);
    append(&w, " lock1="); number(&w, d->lock1);
    append(&w, " lock4="); number(&w, d->lock4);
    append(&w, " lock8="); number(&w, d->lock8);
    append(&w, " maxlock_us="); number(&w, d->maxlock_us);
    append(&w, " sleep4="); number(&w, d->sleep4);
    append(&w, " sleep8="); number(&w, d->sleep8);
    append(&w, " maxsleep_us="); number(&w, d->maxsleep_us);
    log_text(a, b);
    bta_v13_reset(&a->sender_diag);
}
/* V14: record the cost of the HDMI mute keepalive, now out of the audio lock. */
static void v14_hdmi_emit(struct app *a) {
    const struct bta_v14_hdmi *d = &a->hdmi_diag;
    char b[170];
    struct writer w = {b, 0, sizeof(b)};
    append(&w, "FW14 V14 HDMI calls="); number(&w, d->calls);
    append(&w, " slow4="); number(&w, d->slow4);
    append(&w, " slow8="); number(&w, d->slow8);
    append(&w, " maxcall_us="); number(&w, d->maxcall_us);
    append(&w, " errors="); number(&w, d->errors);
    log_text(a, b);
    bta_v14_hdmi_reset(&a->hdmi_diag);
}
static void v8_emit(struct app *a) {
    const struct bta_v8_diag *d = &a->v8;
    char b[320];
    struct writer w = {b, 0, sizeof(b)};
    append(&w, "FW14 V8 METRICS id="); number(&w, a->diag6_stream_id);
    append(&w, " trim_events="); number(&w, d->trim_events);
    append(&w, " trim_ms="); number(&w, (long)(d->trim_bytes / 384UL));
    append(&w, " max_trim_ms="); number(&w, d->max_trim_bytes / 384u);
    append(&w, " underruns="); number(&w, d->underruns);
    append(&w, " resyncs="); number(&w, d->resync_events);
    append(&w, " late4="); number(&w, d->late4);
    append(&w, " late8="); number(&w, d->late8);
    append(&w, " late16="); number(&w, d->late16);
    append(&w, " maxlate_us="); number(&w, d->maxlate_us);
    append(&w, " calls4="); number(&w, d->calls4);
    append(&w, " calls8="); number(&w, d->calls8);
    append(&w, " maxcall_us="); number(&w, d->maxcall_us);
    append(&w, " catchups="); number(&w, d->catchups);
    append(&w, " gain_limited="); number(&w, d->gain_limited);
    append(&w, " gain_pct="); number(&w, bta_v11_gain_pct(a->volume, a->target.address == BTA_V8_FREEPODS_MAC));
    append(&w, " volume_pct="); number(&w, a->volume);
    log_text(a, b);
    a->v9_last_trim_ms = (u32)(d->trim_bytes / 384UL);
    a->v9_last_late8 = d->late8;
    a->v9_last_maxlate_us = d->maxlate_us;
    a->v9_last_calls8 = d->calls8;
    bta_v8_diag_reset(&a->v8);
    char tuned[180];
    struct writer tw = {tuned, 0, sizeof(tuned)};
    append(&tw, "FW14 V9 TUNE cushion_ms="); number(&tw, a->tune.cushion_ms);
    append(&tw, " margin_ms="); number(&tw, a->tune.margin_ms);
    append(&tw, " poll_us="); number(&tw, a->tune.poll_us);
    append(&tw, " stream="); number(&tw, a->diag6_stream_id);
    log_text(a, tuned);
}
/* Under the lock: hand over the pending lines. */
static u32 take_log(struct app *a, char *out) {
    u32 n = a->log_len;
    memcpy(out, a->log, n);
    a->log_len = 0;
    return n;
}
/* Outside the lock: append them to the file. */
static void write_log(struct app *a, const char *b, u32 n) {
    if (!n) return;
    if (a->log_size + n > SERVICE_LOG_CAP) { sc3(SYS_rename, (long)SERVICE_LOG, (long)SERVICE_LOG_OLD, 0); a->log_size = 0; }
    long f = sc3(SYS_open, (long)SERVICE_LOG, 0x209, 0644); /* write, create, append */
    if (f < 0) return;
    sc3(SYS_write, f, (long)b, n);
    closefd(f);
    a->log_size += n;
}

static void message(struct app *a, const char *s) {
    if (!equal(a->message, s)) log_text(a, s);
    memset(a->message, 0, sizeof(a->message));
    u64 n = length(s);
    if (n > 127) n = 127;
    memcpy(a->message, s, n);
}
static const char *target_name(struct app *a) {
    return a->target.name[0] ? a->target.name : "your audio device";
}
static void say(struct app *a, const char *pre, const char *suf) {
    char b[128];
    compose(b, sizeof(b), pre, target_name(a), suf);
    message(a, b);
}
/* On-screen notices only on real transitions; a flapping link must not spam them. */
static int may_announce(struct app *a) {
    u64 now = a->clock();
    if (a->notify_at && now - a->notify_at < 10000000) return 0;
    a->notify_at = now;
    return 1;
}
/* Called from the sender thread, so it does no file I/O. */
static void mute(struct app *a, int on) {
    if (a->hdmi < 0 || a->muted == on) return;
    int v = on;
    a->hdmi_rc = sc3(SYS_ioctl, a->hdmi, HDMI_SET_MUTE, (long)&v);
    if (!a->hdmi_rc) a->muted = on;
}
/* List a device from a search event alone, and hand the entry back. Reading the
 * driver's cache needs kernel offsets (BTA_KCAP_DEVLIST); the event needs none and
 * carries the class of device, all bt_is_audio wants. No name, so the UI shows the
 * MAC — degraded, and far better than an empty list. */
static struct bt_device *note_device(struct app *a, u64 address, u32 cod) {
    for (unsigned i = 0; i < a->devices.count; i++) {
        struct bt_device *e = &a->devices.items[i];
        if (e->address == address) { if (!e->cod) e->cod = cod; return e; }
    }
    if (a->devices.count >= BT_DEVICE_LIMIT) return 0;
    struct bt_device *d = &a->devices.items[a->devices.count++];
    memset(d, 0, sizeof(*d));
    d->address = address; d->cod = cod;
    return d;
}
/* A device answered the search (driver event 1 or 16): remember it with its latest
 * signal strength, and list it when nothing else can (note_device). */
static void note_found(struct app *a, u64 address, int rssi, u32 cod, unsigned event_type) {
    struct bt_device *d = 0;
    if (!(bta_kernel_caps() & BTA_KCAP_DEVLIST)) d = note_device(a, address, cod);
    for (u32 i = 0; i < a->found_count; i++)
        if (a->found[i].address == address) { a->found[i].rssi = rssi; return; }
    if (a->found_count < BT_DEVICE_LIMIT) {
        a->found[a->found_count].address = address;
        a->found[a->found_count].rssi = rssi;
        a->found_count++;
        /* One short log for a newly seen address; no additional driver operation. */
        if (d) diag14_device(a, "scan", d, rssi, 1, event_type);
        else {
            struct bt_device snapshot;
            memset(&snapshot, 0, sizeof(snapshot));
            snapshot.address = address; snapshot.cod = cod;
            diag14_device(a, "scan", &snapshot, rssi, 1, event_type);
        }
    }
}
static int found_rssi(const struct app *a, u64 address, int *rssi) {
    for (u32 i = 0; i < a->found_count; i++)
        if (a->found[i].address == address) { *rssi = a->found[i].rssi; return 1; }
    return 0;
}
/* Stop a running search; the radio rests before the next dial. */
static void end_scan(struct app *a) {
    if (a->scanning) {
        log_num(a, "scan: ended, devices that answered: ", (long)a->found_count);
        if (a->bt >= 0) sc3(SYS_ioctl, a->bt, BT_SCAN_STOP, 0);
        a->radio_quiet_at = a->clock() + CONN_SCAN_QUIET_US;
        a->add_idle = a->clock();
    }
    a->scanning = 0;
}
/* A real link teardown creates a new opportunity for packet-type negotiation.
 * Do NOT reset on a mere game/capture-stream restart: that retains the same BT
 * connection. Preserve inter-command timing and fail-closed disable flag. */
static void fw14_slots_new_link(struct app *a) {
    if (!bta_slots_observe_allowed(a->target.address)) return;
    a->v7_have_last = 0;
    a->v7_observe_at = 0;
    a->v7_observed_mac = 0;
    if (a->slots_exp_budget.requests)
        log_text(a, "FW14 REC5: link cycle; per-link request budget reset");
    bta_slots_recovery_reset_link(&a->slots_exp_budget);
    a->slots_exp_narrow_samples = 0;
    a->slots_exp_wide_samples = 0;
    a->slots_exp_armed = 0;
    a->slots_exp_have_previous = 0;
    a->slots_exp_last_link = 0;
    a->slots_exp_at = 0;
}
static void stop_audio(struct app *a) {
    if (a->state == CONN_DISCOVERING) end_scan(a);
    if (a->streaming) {
        log_num(a, "FW14 DIAG6 stream stop id=", a->diag6_stream_id);
        if (a->diag6_test_prev) {
            log_text(a, "FW14 DIAG6 TEST interrupted by stream stop");
            a->diag6_test_prev = 0;
        }
        log_stats(a, "audio stopped after ");
        short silence[384];
        memset(silence, 0, sizeof(silence));
        struct audio_req q = {a->target.address, silence, sizeof(silence), 0, 0};
        sc3(SYS_ioctl, a->bt, BT_SEND_AUDIO, (long)&q);
    }
    a->streaming = 0; a->level = 0; a->meter = 0; a->meter_peak = 0;
    a->pin_pending = 0; a->test_until = 0; mute(a, 0);
    if (a->ring) { a->unmap(a->ring, 0x10000); a->ring = 0; }
    closefd(a->shmfd); a->shmfd = -1;
}
/* Schedule the next automatic attempt from the backoff schedule (2, 4, 8, 16 s),
 * or give up and wait for the user once it is exhausted. Driven by a->attempts,
 * the count of consecutive failures. */
static void schedule_backoff(struct app *a) {
    u32 n = a->attempts ? a->attempts : 1;
    if (conn_give_up(n)) {
        a->give_up = 1;
        bta_profile_record_fail(&a->profiles, a->target.address);
        a->profile_dirty = 1;
        return;
    }
    a->next_try = a->clock() + conn_backoff_us(n);
}
/* The outstanding dial is over (it failed or ran out its window): the radio rests,
 * then may search or dial again. */
static void page_done(struct app *a) {
    if (!a->page_at) return;
    a->page_at = 0;
    u64 quiet = a->clock() + CONN_SCAN_QUIET_US;
    if (a->radio_quiet_at < quiet) a->radio_quiet_at = quiet;
}
/* A failed attempt: stop, explain, and back off before trying again. */
static void fail(struct app *a, const char *s) {
    page_done(a);
    stop_audio(a); message(a, s); a->dropped = 0; a->state = CONN_RECOVERING;
    schedule_backoff(a);
}
static void fail_named(struct app *a, const char *pre, const char *suf) {
    char m[128];
    compose(m, sizeof(m), pre, target_name(a), suf);
    fail(a, m);
}
/* The PS4 audio stream was rebuilt (e.g. a game started). The headphone link is
 * fine, so this is a quiet reconnect of the audio path, not a failure or a drop. */
static void restart_audio(struct app *a, const char *s) {
    log_text(a, "FW14 DIAG6 capture-stream restart requested");
    stop_audio(a); message(a, s); a->dropped = 0; a->give_up = 0;
    a->state = CONN_RECOVERING; a->next_try = a->clock() + 2000000;
}
/* The Bluetooth link is up but the PS4 audio ring is not ready yet (e.g. right
 * after boot). Keep trying promptly; this is not a connection failure, so it
 * never counts toward the backoff or the give-up. */
static void audio_wait(struct app *a, const char *s) {
    stop_audio(a); message(a, s); a->dropped = 0; a->give_up = 0; a->attempts = 0;
    a->state = CONN_RECOVERING; a->next_try = a->clock() + 3000000;
}
/* Drop the Bluetooth link itself, not just the audio. The driver keeps per-link
 * stream state (e.g. a stuck format renegotiation) that survives an audio stop. */
static void drop_link(struct app *a) {
    if (a->bt >= 0 && a->target.address) {
        u64 address = a->target.address;
        sc3(SYS_ioctl, a->bt, BT_DROP_LINK, (long)&address);
        log_text(a, "link: dropped");
    }
    a->own_link = 0;
    fw14_slots_new_link(a);
}
/* Last-resort recovery once the automatic format fallback has not helped: drop
 * the link and reconnect from scratch, restarting the backoff. Any accepted
 * packet resets the relink count. */
static void relink(struct app *a) {
    a->relinks++; a->relinks_total++; stop_audio(a); drop_link(a);
    a->state = CONN_RECOVERING; a->dropped = 1; a->give_up = 0; a->attempts = 0;
    a->next_try = a->clock() + 2000000; a->refused_since = 0;
    say(a, "The PS4 stopped accepting audio for ", ". Reconnecting...");
}
/* The headphones went away mid-stream, detected only by consecutive audio-send
 * failures (never by polling the driver while streaming: that starved the
 * controller). stop_audio restores TV sound. */
static void link_lost(struct app *a) {
    stop_audio(a); a->dropped = 1; a->give_up = 0; a->attempts = 0;
    a->own_link = 0; /* should the driver still list it, that link is dead: dial afresh */
    fw14_slots_new_link(a);
    a->state = CONN_RECOVERING; a->next_try = a->clock() + 2000000;
    say(a, "", " disconnected. TV sound is back. Reconnecting...");
    if (may_announce(a)) {
        char m[128];
        compose(m, sizeof(m), "", target_name(a), " disconnected. TV sound is back on. Reconnecting...");
        notify(m); a->announced = 1;
    }
}
static struct bt_device *device(struct app *a, u64 address) {
    for (unsigned i = 0; i < a->devices.count; i++)
        if (a->devices.items[i].address == address) return &a->devices.items[i];
    return 0;
}
static int ensure_bt(struct app *a) {
    if (a->bt >= 0) return 0;
    a->bt = sc3(SYS_open, (long)"/dev/bt", 6, 0);
    if (a->bt < 0) return -1;
    u64 mask = 0xffffff;
    sc3(SYS_write, a->bt, (long)&mask, 8);
    return 0;
}
/* The driver's cache has the real names and service bits, so it replaces the list
 * outright. Without the offsets to read it there is nothing to call: note_device
 * lists what the search answers, and the target keeps its profile's name. */
static void refresh_devices(struct app *a) {
    if (!(bta_kernel_caps() & BTA_KCAP_DEVLIST)) return;
    struct bt_devices d;
    if (!bt_list(&d) && d.count <= BT_DEVICE_LIMIT) a->devices = d;
    struct bt_device *t = device(a, a->target.address);
    if (t) a->target = *t;
}
static int scan(struct app *a) {
    /* Never while a dial may still be running (conn.h "Sharing the radio"). */
    if (a->page_at || a->hold) { log_text(a, "scan: not started, a connection attempt holds the radio"); return -1; }
    if (ensure_bt(a)) return -1;
    long rc = sc3(SYS_ioctl, a->bt, BT_SCAN_START, 0);
    log_num(a, "scan: started, rc ", rc); /* the radio is busy for the next 16 s */
    if (rc < 0) return -1;
    a->scanning = 1; a->scan_end = a->clock() + 16000000; a->list_at = 0;
    return 0;
}
struct saved_device { u64 magic; struct bt_device device; };
static int put_profiles(const struct bta_profiles *p) {
    if (put("/data/bta_audio/profiles.tmp", p, sizeof(*p))) return -1;
    if (sc3(SYS_rename, (long)"/data/bta_audio/profiles.tmp", (long)"/data/bta_audio/profiles.bin", 0) < 0) return -1;
    return 0;
}
static int save_profiles(struct app *a) { return put_profiles(&a->profiles); }
/* Load the remembered devices, migrating the pre-refactor device.bin plus the
 * dual-<address> flag into one profile the first time it is seen. */
static void load_profiles(struct app *a) {
    bta_profiles_init(&a->profiles);
    long fd = sc3(SYS_open, (long)"/data/bta_audio/profiles.bin", 0, 0);
    if (fd >= 0) {
        struct bta_profiles p;
        long n = sc3(SYS_read, fd, (long)&p, sizeof(p));
        closefd(fd);
        if (n == (long)sizeof(p) && bta_profiles_valid(&p, sizeof(p))) { a->profiles = p; return; }
    }
    fd = sc3(SYS_open, (long)"/data/bta_audio/device.bin", 0, 0);
    if (fd < 0) return;
    struct saved_device s;
    long n = sc3(SYS_read, fd, (long)&s, sizeof(s));
    closefd(fd);
    if (n == (long)sizeof(s) && s.magic == 0x3254454752415442UL && s.device.address && !(s.device.address >> 48)) {
        s.device.name[95] = 0;
        int i = bta_profile_touch(&a->profiles, &s.device);
        char p[40];
        joint_path(s.device.address, p);
        a->profiles.items[i].mode = exists(p) ? SBC_DUAL : SBC_JOINT;
        a->profiles.items[i].last_ok_us = 1; /* previously used: prefer it on startup */
        save_profiles(a);
    }
}
/* Remember the user's selection and persist the table. */
static int select_device(struct app *a, const struct bt_device *d) {
    bta_profile_touch(&a->profiles, d);
    if (save_profiles(a)) return -1;
    a->target = *d;
    a->mode = bta_profile_mode(&a->profiles, d->address);
    return 0;
}
/* Make the most-recently-used remembered device the initial target so a
 * returning user auto-reconnects; begin_connect re-picks by what is in range. */
static void init_target(struct app *a) {
    int best = -1;
    for (unsigned i = 0; i < a->profiles.count; i++)
        if (best < 0 || a->profiles.items[i].last_ok_us > a->profiles.items[best].last_ok_us) best = (int)i;
    if (best < 0) return;
    bta_profile_device(&a->profiles.items[best], &a->target);
    a->mode = a->profiles.items[best].mode;
    a->wanted = 1;
}
static int save_volume(struct app *a, int volume) {
    if (volume < 0 || volume > BTA_V11_MAX_VOLUME) return -1;
    unsigned value = (unsigned)volume;
    if (put("/data/bta_audio/volume.tmp", &value, sizeof(value))) return -1;
    if (sc3(SYS_rename, (long)"/data/bta_audio/volume.tmp", (long)"/data/bta_audio/volume.bin", 0) < 0) return -1;
    a->volume = volume;
    return 0;
}
static void load_volume(struct app *a) {
    a->volume = 40;
    unsigned value = 101;
    long fd = sc3(SYS_open, (long)"/data/bta_audio/volume.bin", 0, 0);
    if (fd < 0) return;
    long n = sc3(SYS_read, fd, (long)&value, sizeof(value));
    closefd(fd);
    if (n == sizeof(value) && value <= BTA_V11_MAX_VOLUME) a->volume = (int)value;
}
/* Latency mode (BTA_LAT_*): the backlog the sender holds. Persisted so it
 * survives a restart, like the volume. Default balanced (12 ms). */
static void apply_latency(struct app *a, int mode) {
    if (mode < BTA_LAT_LOW || mode > BTA_LAT_STABLE) mode = BTA_LAT_BALANCED;
    u32 old_target = a->pace.target;
    a->latency_mode = mode;
    bta_pace_config(&a->pace, mode);
    /* Existing latency buttons still select their usual cushion sizes.
     * Keep any custom margin and poll interval; the slider follows the preset. */
    a->tune.cushion_ms = a->pace.target / 384u;
    bta_pace_tune(&a->pace, a->tune.cushion_ms, a->tune.margin_ms);
    /* A deeper target needs a deeper cushion; the steady pacer only trims down,
     * so re-enter prebuffering to grow it. Lowering the mode trims on the next
     * send and needs no refill. */
    if (a->pace.target > old_target) a->pace.filling = 1;
}
static int save_latency(struct app *a, int mode) {
    if (mode < BTA_LAT_LOW || mode > BTA_LAT_STABLE) return -1;
    unsigned value = (unsigned)mode;
    if (put("/data/bta_audio/latency.tmp", &value, sizeof(value))) return -1;
    if (sc3(SYS_rename, (long)"/data/bta_audio/latency.tmp", (long)"/data/bta_audio/latency.bin", 0) < 0) return -1;
    apply_latency(a, mode);
    return 0;
}
static void load_latency(struct app *a) {
    unsigned value = BTA_LAT_BALANCED;
    long fd = sc3(SYS_open, (long)"/data/bta_audio/latency.bin", 0, 0);
    if (fd >= 0) {
        long n = sc3(SYS_read, fd, (long)&value, sizeof(value));
        closefd(fd);
        if (n != sizeof(value)) value = BTA_LAT_BALANCED;
    }
    apply_latency(a, (int)value);
}
/* The radio is taken: a search runs or is due, a hold lasts, or it is resting. */
/* Apply one tuning change while the API holds the sender mutex. No flash
 * writes or reconnects. Growing the cushion re-enters prebuffering. */
static void tune_apply(struct app *a, const struct bta_v9_tune *new_tune) {
    u32 old_target = a->pace.target;
    a->tune = *new_tune;
    bta_pace_tune(&a->pace, a->tune.cushion_ms, a->tune.margin_ms);
    if (a->pace.target > old_target && a->streaming) a->pace.filling = 1;
}
static int radio_busy(struct app *a, u64 now) {
    return a->scanning || a->scan_wait || a->hold || now < a->radio_quiet_at;
}
/* Ask the driver for a new link to the target. */
static void dial(struct app *a) {
    u64 address = a->target.address;
    long diag14_scan_stop_rc = sc3(SYS_ioctl, a->bt, BT_SCAN_STOP, 0);
    log_num(a, "DIAG14 pre-dial scan_stop rc=", diag14_scan_stop_rc);
    a->connect_rc = sc3(SYS_ioctl, a->bt, BT_CONNECT, (long)&address);
    log_num(a, "connect: dial rc ", a->connect_rc);
    diag14_dial(a, address, a->connect_rc);
    /* An immediate driver error (e.g. 0x802F0204) means this device will not
     * connect right now; fail fast instead of waiting out the 30 s window. */
    if (a->connect_rc < 0) {
        fail_named(a, "Couldn't connect to ", ". Make sure it's on, nearby and not connected to another device.");
        return;
    }
    a->own_link = address;
    /* bluetooth_tick watches the dial; no search starts until it ends. */
    u64 now = a->clock();
    a->page_at = now; a->page_end = now + CONN_PAGE_END_US; a->deadline = now + CONN_DIAL_WINDOW_US;
    a->page_before = a->page_last = a->btstate; a->page_seen = 0;
}
static void begin_connect(struct app *a) {
    /* Once per run: the patch is idempotent, so every backoff cycle reapplying it
     * bought nothing. Its failing is no reason to stop either — where the patch site
     * wasn't found the device may still link up and play, and the user may find out. */
    if (!a->patch_tried) { a->patch_tried = 1; a->patch_rc = bta_apply_patch(); log_num(a, "patch: accept patch rc ", a->patch_rc); }
    if (ensure_bt(a)) { fail(a, "Bluetooth is not ready. Retrying shortly."); return; }
    refresh_devices(a);
    /* Connect to whichever remembered device is in range: keep the current
     * target if it is present, otherwise pick the best remembered one that is. */
    if (!a->target.address || !device(a, a->target.address)) {
        struct bt_device *p = device(a, bta_profile_pick(&a->profiles, &a->devices));
        if (p) a->target = *p;
    }
    struct bt_device *t = device(a, a->target.address);
    if (!t) {
        if (scan(a)) { fail(a, "Bluetooth scan could not start. Retrying shortly."); return; }
        a->state = CONN_DISCOVERING; a->deadline = a->clock() + 16000000;
        say(a, "Looking for ", "... Put it in pairing mode if needed.");
        return;
    }
    end_scan(a);
    a->mode = bta_profile_mode(&a->profiles, a->target.address);
    u64 address = a->target.address;
    a->btstate = sc3(SYS_ioctl, a->bt, BT_LINK_STATE, (long)&address);
    diag14_device(a, "target", t, 0, 0, 0);
    a->state = CONN_CONNECTING; a->deadline = a->clock() + 30000000; a->redial_at = 0;
    /* An odd class of device is a warning, not a veto: the user picked this one,
     * and a headset that under-reports its profiles can still play. */
    if (!bt_is_audio(t)) message(a, "This device does not advertise Bluetooth audio playback. Trying anyway...");
    else say(a, "Connecting to ", "...");
    log_num(a, "connect: link state ", a->btstate);
    switch (conn_plan_link(a->btstate, a->own_link == address)) {
    case CONN_DIAL: /* right after a search the radio rests first: bluetooth_tick dials then */
        if (radio_busy(a, a->clock())) { a->redial_at = a->clock(); log_text(a, "connect: waiting for the radio"); }
        else dial(a);
        break;
    case CONN_REUSE: a->connect_rc = 0; break;
    case CONN_DROP_FIRST: /* bluetooth_tick dials once it has closed */
        drop_link(a);
        a->redial_at = a->clock() + 2000000;
        break;
    }
}
static void start_audio(struct app *a) {
    a->shmfd = a->shm("/shm_acap.full", 0, 0);
    if (a->shmfd < 0) { log_num(a, "FW14 DIAG6 shm_open failed fd=", a->shmfd); audio_wait(a, "Waiting for the PS4 audio service."); return; }
    void *p = a->map(0, 0x10000, 1, 1, a->shmfd, 0);
    if (!p || p == (void *)-1) { log_text(a, "FW14 DIAG6 mmap failed"); audio_wait(a, "Unable to open the game audio stream."); return; }
    a->ring = p;
    a->readpos = *(volatile u32 *)(a->ring + 12);
    if (*(volatile u32 *)a->ring != 0x10000 || a->readpos < 16 || a->readpos >= 0x10000 || (a->readpos & 7)) {
        log_text(a, "FW14 DIAG6 capture ring header/cursor invalid at stream start");
        audio_wait(a, "The PS4 audio stream has an unexpected format.");
        return;
    }
    /* A truly new UI session starts after Disconnect, not after a game audio
     * stream restart (which retains a->session == 1). */
    if (!a->session) fw14_slots_new_link(a);
    a->streaming = 1; a->state = CONN_STREAMING; a->refused_since = 0; a->errors = 0;
    a->stream_at = a->clock(); a->test_until = 0; bta_v12_tone_reset(&a->tone_clock); a->test_phase = 0; a->snap = 0;
    a->diag6_stream_id++;
    bta_diag6_reset(&a->diag6);
    bta_v8_diag_reset(&a->v8);
    bta_v8_gain_reset(&a->v8_gain);
    a->diag6_at = a->stream_at + 5000000;
    a->diag6_test_prev = 0;
    log_num(a, "FW14 DIAG6 stream start id=", a->diag6_stream_id);
    if (a->v10_reset_pending) {
        log_num(a, "FW14 V10 RESET stream restarted id=", a->diag6_stream_id);
        log_num(a, "FW14 V10 RESET operation kind=", a->v10_reset_kind);
        log_text(a, "FW14 V10 RESET stream restarted; audible playback NOT verified");
        a->v10_reset_pending = 0;
    }
    log_num(a, "FW14 DIAG6 start capture cursor=", a->readpos);
    log_num(a, "FW14 DIAG6 start send type=", conn_send_type(a->mode));
    { char b[140], mac[13]; struct writer w = {b, 0, sizeof(b)};
      mac_text(a->target.address, mac);
      append(&w, "FW14 V7 STREAM mac="); append(&w, mac);
      append(&w, " sbc_send_type="); number(&w, conn_send_type(a->mode));
      append(&w, " hci_experiment="); number(&w, bta_slots_command_target(a->target.address));
      log_text(a, b); }
    a->stats_at = a->stream_at + 600000000; a->stream_packets = a->packets; a->stream_resyncs = a->resyncs;
    a->stream_events = 0;
    bta_pace_reset(&a->pace, a->stream_at); a->drops = 0; a->underruns = 0;
    int was_dropped = a->dropped, told = a->announced, fresh = !a->session;
    a->attempts = 0; a->give_up = 0; a->dropped = 0; a->announced = 0; a->session = 1;
    /* relinks persists across relink cycles (reset only when a packet is finally
     * accepted, in audio_tick) so the give-up cap can trigger. */
    a->confirmed = 0; a->modes_tried = (u8)(1 << a->mode);
    a->ok_at = a->stream_at; a->meter = 0; a->meter_peak = 0;
    say(a, "Connected to ", ".");
    /* One TV notice per real transition: a link that dropped after it was announced
     * is back, or this session's first connection succeeded. */
    if ((was_dropped ? told : fresh) && may_announce(a)) {
        char m[128];
        if (was_dropped) compose(m, sizeof(m), "", target_name(a), " reconnected.");
        else compose(m, sizeof(m), "Connected to ", target_name(a), ".");
        notify(m);
    }
}
/* Let go of the device: drop its link, which also cancels a dial still running,
 * and hold the radio until the driver shows that done (radio_tick). */
static void release_link(struct app *a) {
    u64 address = a->target.address, now = a->clock();
    int dialling = a->page_at != 0;
    a->page_at = 0; a->redial_at = 0;
    if (a->bt < 0 || !address) { a->own_link = 0; return; }
    long st = sc3(SYS_ioctl, a->bt, BT_LINK_STATE, (long)&address);
    sc3(SYS_ioctl, a->bt, BT_DROP_LINK, (long)&address);
    a->own_link = 0;
    /* The user requested physical disconnect. Reset the request budget for
     * the next link, not for any audio-only restart. */
    if (conn_link_up(st)) fw14_slots_new_link(a);
    enum conn_hold h = conn_link_up(st) ? CONN_HOLD_LINK : dialling ? CONN_HOLD_PAGE : CONN_HOLD_NONE;
    if (h == CONN_HOLD_LINK) log_text(a, "link: dropped");
    if (h == CONN_HOLD_PAGE) log_num(a, "radio: cancelling the dial, link state ", st);
    if (h && !a->hold) { a->hold = h; a->hold_at = now; a->hold_addr = address; a->hold_poll = now + 250000; a->page_last = st; }
}
/* Add Device (POST /api/scan): stop reconnecting, let go of the device (its audio
 * and link, or its dial), and search once the radio is free. Asked again while it
 * lasts: one more search, once the running one is over. */
static void begin_add(struct app *a, enum conn_add who) {
    a->add_idle = a->clock();
    if (a->adding) { if (!a->scanning) a->scan_wait = 1; return; }
    a->found_count = 0; /* list who answers from now on */
    int playing = a->streaming, dialling = a->page_at != 0;
    a->adding = who;
    a->resume = a->wanted && !a->give_up;
    log_text(a, who == CONN_ADD_CONSOLE ? "add: Add Device opened" : "add: a phone asked for a search");
    if (a->state == CONN_DISCOVERING) { a->state = CONN_IDLE; log_text(a, "add: using the search already running"); }
    else if (a->wanted && !playing && a->state != CONN_IDLE) log_text(a, "add: reconnecting stopped");
    stop_audio(a);
    a->wanted = 0; a->pin_pending = 0; a->state = CONN_IDLE;
    a->dropped = a->give_up = a->announced = 0;
    release_link(a);
    if (playing) say(a, "", " is disconnected while you add a device. TV sound is back.");
    else if (dialling) say(a, "Stopped connecting to ", " while you add a device.");
    a->scan_wait = !a->scanning;
}
/* Add Device ends. resume: reconnect the device it paused (it was left, or the app
 * went away); otherwise the caller decides what happens next. */
static void end_add(struct app *a, const char *why, int resume) {
    if (!a->adding) return;
    log_text(a, why);
    end_scan(a);
    a->adding = CONN_ADD_NONE; a->scan_wait = 0;
    if (resume && a->resume && a->target.address) {
        a->wanted = 1; a->state = CONN_RECOVERING; a->next_try = 0; a->attempts = 0;
        a->dropped = a->give_up = a->announced = a->session = 0;
        say(a, "Reconnecting to ", "...");
    }
    a->resume = 0;
}
/* Every tick: settle a hold, start the waiting search, end Add Device when its
 * owner went away (conn.h "Sharing the radio"). */
static void radio_tick(struct app *a, u64 now) {
    if (a->hold && now >= a->hold_poll) {
        a->hold_poll = now + 250000;
        u64 address = a->hold_addr;
        long st = a->bt >= 0 ? sc3(SYS_ioctl, a->bt, BT_LINK_STATE, (long)&address) : -1;
        if (a->hold == CONN_HOLD_PAGE && st != a->page_last) { log_num(a, "radio: link state ", st); a->page_last = st; }
        enum conn_hold_step step = conn_hold_next(a->hold, st, a->page_before, a->page_seen, now, a->hold_at, a->page_end);
        if (step == CONN_HOLD_LINKED) {
            if (a->bt >= 0) sc3(SYS_ioctl, a->bt, BT_DROP_LINK, (long)&address);
            log_text(a, "radio: the dial linked up anyway; link dropped");
            a->hold = CONN_HOLD_LINK; a->hold_at = now;
        } else if (step != CONN_HOLD_WAIT) {
            u64 waited = now - a->hold_at;
            if (step == CONN_HOLD_CANCELLED) log_ms(a, "radio: dial cancelled after ", waited, ", the driver confirmed it");
            else if (step == CONN_HOLD_WINDOW_OVER) log_ms(a, "radio: waited ", waited, " for the dial to end (no confirmation)");
            else if (step == CONN_HOLD_DOWN) log_ms(a, "radio: link down after ", waited, "");
            else {
                log_text(a, "radio: the link stayed up");
                if (a->scan_wait) { a->scan_wait = 0; say(a, "Couldn't disconnect ", ". Press Search Again to try again."); }
            }
            a->hold = CONN_HOLD_NONE;
            u64 quiet = now + CONN_HOLD_QUIET_US;
            if (a->radio_quiet_at < quiet) a->radio_quiet_at = quiet;
        }
    }
    if (a->scan_wait && !a->hold && now >= a->radio_quiet_at) {
        a->scan_wait = 0;
        if (!a->scanning && scan(a)) message(a, "Bluetooth search could not start. Press Search Again.");
    }
    if (a->adding && conn_add_over(a->adding, now, a->console_at, a->add_idle, a->scanning || a->scan_wait))
        end_add(a, a->adding == CONN_ADD_CONSOLE ? "add: ended, the app went away" : "add: ended, no device was chosen", 1);
}
/* One message from the driver (read from /dev/bt). */
struct bt_event { u8 type, subtype; u16 reserved; u32 data; u64 address; };
/* Driver events go to service.log: the service answers two kinds (3: PIN wanted,
 * 4: accept pairing), and what a device that won't connect or play is asking for
 * would be among the rest. At most 12 lines in 10 s, and while audio plays only
 * the stream's first 20, so a chatty link can't push everything else out. */
static void log_event(struct app *a, const struct bt_event *e, u64 now) {
    if (a->streaming && a->stream_events++ >= 20) return;
    if (now >= a->event_at + 10000000) { a->event_at = now; a->event_lines = 0; }
    if (a->event_lines++ >= 12) return;
    char b[96], mac[13];
    struct writer w = {b, 0, sizeof(b)};
    append(&w, "bt event: type ");
    number(&w, e->type);
    append(&w, ", subtype ");
    number(&w, e->subtype);
    append(&w, ", data ");
    number(&w, (long)e->data);
    mac_text(e->address, mac);
    append(&w, e->address == a->target.address ? ", the device " : ", from ");
    append(&w, mac);
    log_text(a, b);
}
/* V7: selected-device link telemetry. Snapshot is already copied out by the
 * existing guarded FW14 callback; this formatting does NOT call /dev/bt or
 * inspect new kernel structures. It cannot establish RF delivery or codec. */
static void v7_link_observation(struct app *a, const struct bta_slots_sample *s,
                                u64 now) {
    int changed = !a->v7_have_last || a->v7_observed_mac != a->target.address ||
        s->status != a->v7_last.status || s->link_address != a->v7_last.link_address ||
        s->mode != a->v7_last.mode || s->maxslots != a->v7_last.maxslots ||
        s->channel != a->v7_last.channel || s->active != a->v7_last.active ||
        s->pktmask != a->v7_last.pktmask || s->submitted != a->v7_last.submitted;
    if (!changed && now < a->v7_observe_at) return;
    char b[320], mac[13];
    struct writer w = {b, 0, sizeof(b)};
    mac_text(a->target.address, mac);
    append(&w, "FW14 V7 LINK mac="); append(&w, mac);
    append(&w, " status="); number(&w, s->status);
    append(&w, " hci_mode="); number(&w, s->mode);
    append(&w, " slots="); number(&w, s->maxslots);
    append(&w, " pktmask="); diag14_hex32(&w, s->pktmask);
    append(&w, " channel="); number(&w, s->channel);
    append(&w, " service="); number(&w, s->active);
    append(&w, " pending="); number(&w, s->pending);
    append(&w, " countdown="); number(&w, s->countdown);
    append(&w, " pcm="); number(&w, s->pcm);
    append(&w, " submit="); number(&w, s->submitted);
    append(&w, " handle="); number(&w, s->handle);
    append(&w, " hci_experiment="); number(&w, bta_slots_command_target(a->target.address));
    log_text(a, b);
    a->v7_last = *s;
    a->v7_observed_mac = a->target.address;
    a->v7_have_last = 1;
    a->v7_observe_at = now + 5000000;
}
/* FW14 V5: restore a one-slot link only after it is safe to request the
 * firmware-native packet type. A successful wide transition (two samples)
 * re-arms recovery; one-slot reappearance, including after an audio route
 * change / re-link, can trigger another single request. No Exit Sniff here. */
static void fw14_slots_tick(struct app *a, u64 now) {
    if (!a->streaming || !bta_slots_observe_allowed(a->target.address) ||
        a->slots_exp_disabled || now < a->slots_exp_at) return;
    a->slots_exp_at = now + 1000000; /* 1 read-only snapshot per second */

    struct bta_slots_sample sample;
    int rc = bt_fw14_slots_experiment(a->target.address, 0, &sample);
    if (rc) {
        log_num(a, "FW14 REC5: observer failed rc=", rc);
        a->slots_exp_disabled = 1;
        return;
    }
    v7_link_observation(a, &sample, now);
    if (sample.status == BTA_SLOTS_UNSUPPORTED ||
        sample.status == BTA_SLOTS_BAD_TARGET) {
        log_num(a, "FW14 REC5: unsupported/status=", sample.status);
        a->slots_exp_disabled = 1;
        return;
    }
    if (sample.status != BTA_SLOTS_OBSERVE) {
        a->slots_exp_narrow_samples = 0;
        a->slots_exp_wide_samples = 0;
        if (a->slots_exp_have_previous) {
            log_num(a, "FW14 REC5: link status=", sample.status);
            a->slots_exp_have_previous = 0;
        }
        return;
    }

    if (!a->slots_exp_have_previous ||
        sample.maxslots != a->slots_exp_previous_slots ||
        sample.mode != a->slots_exp_previous_mode ||
        sample.link_address != a->slots_exp_last_link) {
        log_num(a, "FW14 REC5: observed mode=", sample.mode);
        log_num(a, "FW14 REC5: observed maxslots=", sample.maxslots);
        log_num(a, "FW14 REC5: observed channel=", sample.channel);
        log_num(a, "FW14 REC5: observed audio service=", sample.active);
        a->slots_exp_have_previous = 1;
        a->slots_exp_previous_slots = sample.maxslots;
        a->slots_exp_previous_mode = sample.mode;
        a->slots_exp_last_link = sample.link_address;
    }

    if (sample.maxslots >= 3 && (sample.active & 8u) && sample.channel == 10) {
        if (a->slots_exp_wide_samples < 2) a->slots_exp_wide_samples++;
        a->slots_exp_narrow_samples = 0;
        if (a->slots_exp_wide_samples == 2 && !a->slots_exp_armed) {
            a->slots_exp_armed = 1;
            log_text(a, "FW14 REC5: confirmed wide; recovery rearmed");
        }
        return;
    }
    a->slots_exp_wide_samples = 0;
    /* Count one-slot observations even in Sniff; issue only after Active.
     * This preserves a brief Active-mode opportunity during reconnection. */
    if (bta_slots_recovery_narrow(&sample)) {
        if (a->slots_exp_narrow_samples < 2) a->slots_exp_narrow_samples++;
    } else {
        a->slots_exp_narrow_samples = 0;
    }
    /* Default V7 diagnostic build is strictly read-only for SpaceBuds.
     * FreePods 4 keeps its original V5 recovery policy unchanged. */
    if (!bta_slots_command_target(a->target.address)) return;
    if (!bta_slots_recovery_ready(&sample, &a->slots_exp_budget,
            a->slots_exp_armed, a->slots_exp_narrow_samples, now)) return;

    /* Revalidate inside kernel immediately before issuing the HCI command. */
    bta_slots_recovery_record_request(&a->slots_exp_budget, now);
    a->slots_exp_armed = 0; /* never retry until wide state was confirmed */
    a->slots_exp_narrow_samples = 0;
    struct bta_slots_sample result;
    rc = bt_fw14_slots_experiment(a->target.address, 1, &result);
    log_num(a, "FW14 REC5: per-link attempt=", a->slots_exp_budget.requests);
    log_num(a, "FW14 REC5: command helper rc=", rc);
    if (a->target.address == BTA_FW14_SPACESBUDS_MAC)
        log_text(a, "FW14 V7 EXPERIMENT SpaceBuds packet-type command attempted");
    if (rc) {
        a->slots_exp_disabled = 1;
        return;
    }
    log_num(a, "FW14 REC5: HCI packet-type status=", result.status);
    log_num(a, "FW14 REC5: HCI queue rc (not completion)=", result.command_rc);
    log_num(a, "FW14 REC5: before maxslots=", result.maxslots);
    if (result.status != BTA_SLOTS_SENT) {
        a->slots_exp_disabled = 1; /* fail closed; never blindly retry */
        log_text(a, "FW14 REC5: no command queued; disabled for this run");
    } else if (a->slots_exp_budget.requests >= BTA_SLOTS_SESSION_CAP) {
        log_text(a, "FW14 REC5: per-link request cap reached (resets on real reconnect)");
    }
}
static void bluetooth_tick(struct app *a, u64 now) {
    /* While audio is streaming, do NOT walk the driver cache: refresh_devices()
     * takes the kernel's Bluetooth link lock, and polling it mid-stream starves
     * the wireless controller's HID link. The proven BTA bridge never did this.
     * Device enumeration resumes as soon as streaming stops. */
    if (!a->streaming && now >= a->list_at) {
        refresh_devices(a);
        a->list_at = now + (a->scanning ? 500000 : 3000000);
    }
    if (a->scanning && now >= a->scan_end) end_scan(a);
    if (a->bt >= 0) {
        struct bt_event ev[16];
        long n = sc3(SYS_read, a->bt, (long)ev, sizeof(ev));
        if (n > 0 && n <= (long)sizeof(ev))
            for (unsigned i = 0; i < (unsigned)n / sizeof(ev[0]); i++) {
                /* A device answering the search: 1 with its class of device as
                 * data, 16 after it; the subtype is the signal strength (dBm, as a
                 * signed byte). Dozens per search, so recorded, not logged. */
                if (ev[i].type == 1 || ev[i].type == 16) { note_found(a, ev[i].address, (signed char)ev[i].subtype, ev[i].data, ev[i].type); continue; }
                log_event(a, &ev[i], now);
                /* Pairing only happens during connection setup. Ignoring these
                 * while streaming keeps the state == CONN_STREAMING invariant that
                 * link_state() relies on, and avoids a stray event knocking a live
                 * stream back into the forbidden link-state poll. */
                if (ev[i].address == a->target.address && a->wanted && !a->streaming) {
                    if (ev[i].type == 4) { u64 q[2] = {a->target.address, 1};
                        long diag14_pair_rc = sc3(SYS_ioctl, a->bt, BT_PAIR_ACCEPT, (long)q);
                        log_num(a, "DIAG14 pair_accept rc=", diag14_pair_rc); }
                    if (ev[i].type == 3) { a->pin_pending = 1; a->state = CONN_PAIRING; a->deadline = now + 60000000; message(a, "This device requires a pairing PIN."); }
                }
            }
    }
    radio_tick(a, now);
    if (!a->wanted) return;
    if (a->state == CONN_RECOVERING) {
        if (!a->give_up && now >= a->next_try && !radio_busy(a, now)) { a->attempts++; begin_connect(a); }
        return;
    }
    if (a->state == CONN_DISCOVERING) {
        if (device(a, a->target.address)) { end_scan(a); begin_connect(a); return; }
        struct bt_device *p = device(a, bta_profile_pick(&a->profiles, &a->devices));
        if (p) { a->target = *p; end_scan(a); begin_connect(a); return; }
        if (now >= a->deadline) fail_named(a, "Couldn't find ", ". Put it in pairing mode.");
        return;
    }
    if (a->state == CONN_CONNECTING || a->state == CONN_PAIRING) {
        u64 address = a->target.address;
        if (a->redial_at) {
            /* begin_connect dropped a stale link, or found the radio resting after a
             * search: dial once the link had time to close and the radio is free. */
            if (now < a->redial_at || radio_busy(a, now)) return;
            a->redial_at = 0;
            a->btstate = sc3(SYS_ioctl, a->bt, BT_LINK_STATE, (long)&address);
            if (!conn_link_up(a->btstate)) { dial(a); return; }
            /* Up again already: the device linked back by itself, on a new link. */
            log_text(a, "connect: the device linked back by itself");
            a->own_link = address;
        }
        a->btstate = sc3(SYS_ioctl, a->bt, BT_LINK_STATE, (long)&address);
        if (a->page_at) {
            /* Whether the driver shows the dial in its link state decides if a
             * cancel can ever be confirmed (radio_tick), so each change is logged. */
            if (a->btstate != a->page_last) { log_num(a, "connect: link state ", a->btstate); a->page_last = a->btstate; }
            if (conn_paging(a->btstate, a->page_before)) a->page_seen = 1;
        }
        if (conn_link_up(a->btstate)) {
            a->page_at = 0;
            log_text(a, "connect: link up");
            a->pin_pending = 0;
            /* Widening only asks the controller for roomier packet types; the link
             * carries audio without it, so a failure is logged, never fatal. */
            if (!(bta_kernel_caps() & BTA_KCAP_WIDEN)) a->packet_rc = 0;
            else if ((a->packet_rc = bt_prepare(address)) != 0) log_num(a, "connect: 3-slot widening rc ", a->packet_rc);
            a->state = CONN_CONFIGURING; a->deadline = now + 400000;
        } else if (a->state == CONN_CONNECTING && a->page_at && conn_dial_ended(a->btstate, a->page_before, a->page_seen)) {
            /* The driver gave up on it (after about 15 s): try again now rather
             * than wait out the rest of the window. */
            log_ms(a, "connect: the driver ended the dial after ", now - a->page_at, "");
            fail_named(a, "", " didn't respond. Is it on and nearby?");
        } else if (now >= a->deadline) {
            fail_named(a, "", " didn't respond. Is it on and nearby?");
        }
    } else if (a->state == CONN_CONFIGURING && now >= a->deadline) start_audio(a);
}
static void roll_second(struct app *a, u64 now) {
    bta_roll_second(a->secs, &a->cur, &a->sec_seq, &a->sec_start, now);
}
/* Send type 4 is joint stereo at full bitpool, the format headphones agree to at
 * connect; type 3 is dual channel, which the driver renegotiates. The mode is
 * remembered per device (profiles.h) and auto-switched on silence below. */
static void audio_tick(struct app *a, u64 now) {
    if (!a->streaming) { a->last_send = 0; return; }
    roll_second(a, now);
    short pcm[384];
    unsigned peak = 0;
    unsigned backlog = 0;
    int diag6_tone = now < a->test_until;
    if (diag6_tone) {
        /* V12: anchor the test tone to a 4ms absolute clock rather than
         * accumulating poll latency on every packet as V11 did. */
        if (!bta_v12_tone_ready(&a->tone_clock, now)) return;
        bta_test_tone(pcm, &a->test_phase, bta_v11_base_volume(a->volume), &a->level, &peak);
    } else {
        u32 writepos = (u32)(*(volatile u64 *)(a->ring + 8) >> 32);
        if (writepos < 16 || writepos >= 0x10000 || (writepos & 7)) {
            restart_audio(a, "Audio stream changed. Reconnecting...");
            return;
        }
        /* The test tone does not read the capture ring, so a long test laps it
         * and leaves readpos stale. On the first capture read after one, drop to
         * the live write cursor and prebuffer afresh instead of sending old or
         * lapped audio. */
        if (a->snap) { a->readpos = writepos; a->snap = 0; bta_pace_reset(&a->pace, now); }
        unsigned available = bta_ring_available(writepos, a->readpos);
        /* Clock-paced send with a bounded backlog (see pacing.h): prebuffer the
         * target cushion, then one packet per ~4 ms, dropping the oldest bytes
         * when the backlog runs ahead, instead of bursting and resyncing only at
         * ~128 ms. WAIT also covers the prebuffer, so it is not an underrun. */
        u64 due_before = a->pace.due;
        int filling_before = a->pace.filling;
        struct bta_pace_step step = bta_pace_run(&a->pace, now, available);
        if (step.kind == BTA_PACE_WAIT) return;
        bta_v8_note_pacer(&a->v8, step.kind, step.skip, filling_before,
                          now, due_before, step.catchup);
        if (step.kind == BTA_PACE_UNDERRUN) { a->underruns++; return; }
        if (step.skip) {
            a->readpos = bta_ring_advance(a->readpos, step.skip);
            if (step.kind == BTA_PACE_RESYNC) a->resyncs++; else a->drops++;
        }
        backlog = available - step.skip; /* audio actually held and sent, after any trim */
        a->readpos = bta_convert_capture(a->ring, a->readpos, bta_v11_base_volume(a->volume), pcm, &a->level, &peak);
    }
    /* V11 extends the UI slider beyond 100% with a peak-limited PCM boost.
     * At <=100%, the prior V10 path is unchanged for either headset. */
    if (bta_v11_apply_volume(&a->v8_gain, pcm, &a->level, &peak,
                             a->volume, a->target.address == BTA_V8_FREEPODS_MAC))
        a->v8.gain_limited++;
    struct audio_req req = {a->target.address, pcm, sizeof(pcm), (u32)conn_send_type(a->mode), 0};
    a->last_rc = sc3(SYS_ioctl, a->bt, BT_SEND_AUDIO, (long)&req);
    {
        u64 done = a->clock();
        bta_diag6_note(&a->diag6, diag6_tone, a->last_rc, peak, now, done);
        if (diag6_tone) bta_v12_tone_note_result(&a->tone_clock, now, done, a->last_rc);
        bta_v8_note_call(&a->v8, now, done);
        struct bta_second *s = &a->cur;
        s->pkts++;
        if (a->last_rc == 0) s->refused++;
        else if (a->last_rc < 0) s->failed++;
        if (a->last_send) {
            u64 gap = now - a->last_send;
            if (gap > s->maxgap_us) s->maxgap_us = (u32)(gap > 0xffffffffUL ? 0xffffffffUL : gap);
            if (gap > 8000) s->late8++;
            if (gap > 16000) s->late16++;
            if (gap > 50000) s->late50++;
        }
        if (done - now > s->maxsend_us) s->maxsend_us = (u32)(done - now);
        if (backlog / 384 > s->maxback_ms) s->maxback_ms = backlog / 384;
        if (peak > s->peak) s->peak = peak;
        a->last_send = now;
    }
    if (a->last_rc == 0) {
        /* Connected but the PS4 is refusing audio: the "connected yet silent"
         * case. First try the other SBC format once, keeping the link (the step
         * that used to need a manual reconnect); then fall back to a full relink,
         * and finally ask the user to power-cycle the headphones. */
        if (!a->refused_since) a->refused_since = now;
        else if (now - a->refused_since > REFUSED_US) {
            int other = conn_other_mode(a->mode);
            if (!(a->modes_tried & (1 << other))) {
                log_num(a, "DIAG14 audio: old send type=", conn_send_type(a->mode));
                log_num(a, "DIAG14 audio: last send rc=", a->last_rc);
                a->mode = other; a->modes_tried |= (u8)(1 << other); a->refused_since = 0;
                log_num(a, "DIAG14 audio: new send type=", conn_send_type(a->mode));
                say(a, "No sound yet from ", ". Trying the other audio format...");
            } else if (a->relinks < 2) {
                relink(a); return;
            } else if (a->relinks == 2) {
                a->relinks++;
                say(a, "The PS4 keeps refusing audio for ", ". Turn the device off and on.");
            }
        }
    } else if (a->last_rc > 0) {
        a->refused_since = 0; a->relinks = 0;
        /* A packet was accepted: remember the mode that works for this device. */
        if (!a->confirmed) {
            a->confirmed = 1;
            bta_profile_record_ok(&a->profiles, a->target.address, a->mode, now);
            a->profile_dirty = 1;
        }
    }
    if (a->last_rc < 0) {
        if (++a->errors > 20) link_lost(a);
    } else {
        a->errors = 0; a->packets++; a->ok_at = now;
        if (peak > a->meter_peak) a->meter_peak = peak;
        /* Prime the mix: hold HDMI unmuted for PRIME_US after stream start, then mute. */
        int want = !a->tv;
        if (want && now - a->stream_at < PRIME_US) want = 0;
        if (a->muted != want) mute(a, want);
    }
}
/* The one place the connection's real state is decided (see conn.h). A UI must
 * show this, never infer it from "selected" or "wanted". */
static int link_state(struct app *a) {
    if (conn_due(a->state, a->dropped, a->give_up, a->next_try, a->clock())) return BT_LINK_CONNECTING;
    return conn_link_state(a->state, a->dropped, a->give_up);
}
static int retry_in_ms(struct app *a, u64 now) {
    return conn_retry_in_ms(a->state, a->give_up, a->next_try, now);
}
static int quiet_for_ms(struct app *a, u64 now) {
    return conn_quiet_for_ms(a->state, a->ok_at, now);
}
/* reveal: include the live pairing code. Set only for a loopback HTTP request,
 * so a future console app can read it; never for a LAN response, and never for
 * the on-disk status.json (pair_code.txt is the one file that holds the code). */
static unsigned status(struct app *a, char *out, unsigned cap, int reveal) {
    u64 now = a->clock();
    struct writer w = {out, 0, cap};
    append(&w, "{\"version\":\"3.0.0\",\"api\":");
    number(&w, BTA_API_VERSION);
    append(&w, ",\"message\":");
    quoted(&w, a->message);
    field(&w, "sender", a->sender_alive);
    field(&w, "sender_prio", a->sender_prio);
    field(&w, "thread_rc", a->thread_rc);
    field(&w, "prio_rc", a->prio_rc);
    field(&w, "aff_rc", a->aff_rc);
    field(&w, "pi_rc", a->pi_rc);
    field(&w, "volume", a->volume);
    field(&w, "volume_max", BTA_V11_MAX_VOLUME);
    field(&w, "volume_post_gain_pct", bta_v11_gain_pct(a->volume, a->target.address == BTA_V8_FREEPODS_MAC));
    field(&w, "wanted", a->wanted);
    field(&w, "streaming", a->streaming);
    field(&w, "v10_reset_count", a->v10_reset_count);
    field(&w, "v10_reset_kind", a->v10_reset_kind);
    field(&w, "v10_reset_pending", a->v10_reset_pending);
    field(&w, "v10_reset_wait_ms", bta_v10_reset_wait_ms(now, a->v10_reset_next_at));
    field(&w, "tv_enabled", a->tv);
    field(&w, "joint", a->mode == SBC_JOINT);
    field(&w, "send_type", conn_send_type(a->mode));
    field(&w, "relinks", a->relinks_total);
    field(&w, "latency_mode", a->latency_mode);
    field(&w, "tune_cushion_ms", a->tune.cushion_ms);
    field(&w, "tune_margin_ms", a->tune.margin_ms);
    field(&w, "tune_poll_us", a->tune.poll_us);
    field(&w, "tune_trim_5s_ms", a->v9_last_trim_ms);
    field(&w, "tune_late8_5s", a->v9_last_late8);
    field(&w, "tune_maxlate_5s_us", a->v9_last_maxlate_us);
    field(&w, "tune_calls8_5s", a->v9_last_calls8);
    field(&w, "drops", a->drops);
    field(&w, "underruns", a->underruns);
    field(&w, "refused_ms", a->refused_since ? (long)((now - a->refused_since) / 1000) : 0);
    field(&w, "hdmi_muted", a->muted == 1);
    field(&w, "scanning", a->scanning || a->scan_wait); /* a search waiting for the radio counts */
    field(&w, "adding", a->adding != CONN_ADD_NONE);
    field(&w, "pin_pending", a->pin_pending);
    field(&w, "test", now < a->test_until);
    field(&w, "link", link_state(a));
    append(&w, ",\"state\":");
    quoted(&w, conn_state_name(a->state));
    field(&w, "attempts", a->attempts);
    field(&w, "retry_ms", retry_in_ms(a, now));
    field(&w, "meter", a->meter);
    field(&w, "quiet_ms", quiet_for_ms(a, now));
    field(&w, "level", a->level);
    field(&w, "packets", a->packets);
    field(&w, "errors", a->errors);
    field(&w, "resyncs", a->resyncs);
    field(&w, "patch_rc", a->patch_rc);
    field(&w, "packet_rc", a->packet_rc);
    field(&w, "connect_rc", a->connect_rc);
    field(&w, "audio_rc", a->last_rc);
    field(&w, "hdmi_rc", a->hdmi_rc);
    field(&w, "bt_state", a->btstate);
    /* What the kernel yielded here: the line a bug report quotes, and the parts a UI acts on. */
    const struct bta_kernel *k = bta_kernel_info();
    unsigned sigs = 0;                                                /* of the six signatures, */
    for (unsigned i = 0; k && i < BTA_SIG_ROUTINES; i++) if (!k->sig_flag[i]) sigs++;
    append(&w, ",\"kernel\":");
    quoted(&w, a->kernel);
    field(&w, "fw_version", k ? (long)k->fw_version : 0);
    field(&w, "kernel_caps", (long)bta_kernel_caps());
    field(&w, "kernel_sigs", sigs);
    field(&w, "uptime_us", a->clock());
    field(&w, "lan", a->auth.enabled);
    if (a->auth.enabled && a->ip) {
        char ip[16];
        ip_text(a->ip, ip);
        append(&w, ",\"ip\":");
        quoted(&w, ip);
    }
    if (reveal && a->auth.enabled) {
        append(&w, ",\"pair_code\":");
        quoted(&w, a->auth.code);
    }
    char mac[13];
    mac_text(a->target.address, mac);
    append(&w, ",\"selected\":");
    quoted(&w, mac);
    append(&w, ",\"name\":");
    quoted(&w, a->target.name);
    append(&w, ",\"devices\":[");
    /* Every device, audio or not: one with an odd class of device is exactly the one
     * a user must see to try it. "audio" is for labelling, never for hiding. */
    for (unsigned i = 0; i < a->devices.count; i++) {
        struct bt_device *d = &a->devices.items[i];
        if (i) append(&w, ",");
        mac_text(d->address, mac);
        append(&w, "{\"address\":");
        quoted(&w, mac);
        append(&w, ",\"name\":");
        quoted(&w, d->name);
        field(&w, "audio", bt_is_audio(d)); /* services bits, or the class of device alone */
        field(&w, "known", bta_profile_known(&a->profiles, d->address));
        int rssi = 0, found = found_rssi(a, d->address, &rssi);
        field(&w, "found", found); /* always both, so a reader never takes them from the next device */
        field(&w, "rssi", rssi);
        append(&w, "}");
    }
    append(&w, "]}");
    return w.used;
}
static unsigned timing_json(struct app *a, char *out, unsigned cap) {
    return bta_timing_json(a->secs, a->sec_seq, (long)(a->clock() / 1000000), out, cap);
}
/* Write the response header for an n-byte body into output; returns its length. */
static unsigned response_head(struct client *c, const char *code, const char *type, unsigned n) {
    struct writer w = {c->output, 0, sizeof(c->output)};
    append(&w, "HTTP/1.1 ");
    append(&w, code);
    append(&w, "\r\nContent-Type: ");
    append(&w, type);
    append(&w, "\r\nContent-Length: ");
    number(&w, n);
    append(&w, "\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nContent-Security-Policy: default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; frame-ancestors 'none'\r\n\r\n");
    return w.used;
}
static void response(struct client *c, const char *code, const char *type, const char *body, unsigned n) {
    unsigned h = response_head(c, code, type, n);
    /* Never drop a reply silently: the client would wait out the deadline and see
     * the connection closed with nothing. */
    if (h + n > sizeof(c->output)) { response(c, "500 Internal Server Error", "text/plain", "Response too large.", 19); return; }
    memcpy(c->output + h, body, n);
    c->head = c->total = h + n;
    c->tail = 0;
}
/* A constant body of any size (the page) goes out from where it lives, after the header. */
static void response_const(struct client *c, const char *code, const char *type, const char *body, unsigned n) {
    c->head = response_head(c, code, type, n);
    c->tail = body;
    c->total = c->head + n;
}
static char *header(char *request, const char *name) {
    char *p = find(request, "\r\n");
    if (!p) return 0;
    p += 2;
    while (*p && *p != '\r') {
        char *q = p;
        const char *k = name;
        while (*k) {
            char c = *q;
            if (c >= 'A' && c <= 'Z') c += 32;
            if (c != *k) break;
            q++; k++;
        }
        if (!*k && *q == ':') { q++; while (*q == ' ' || *q == '\t') q++; return q; }
        p = find(p, "\r\n");
        if (!p) return 0;
        p += 2;
    }
    return 0;
}
static int host_valid(char *request) {
    char *p = header(request, "host");
    if (!p) return 0;
    u8 ip[4];
    for (int i = 0; i < 4; i++) {
        unsigned n = 0, digits = 0;
        while (*p >= '0' && *p <= '9') { n = n * 10 + *p++ - '0'; if (++digits > 3 || n > 255) return 0; }
        if (!digits) return 0;
        ip[i] = n;
        if (i < 3 && *p++ != '.') return 0;
    }
    return local_ip(ip) && starts(p, ":8097\r\n");
}
/* Copy the X-BTA-Token header value (hex, up to a token's length) into a
 * NUL-terminated buffer. The raw header value runs up to a CR, so it must be
 * bounded here before any string compare. */
static void read_token(char *request, char out[BTA_TOKEN_HEX + 1]) {
    out[0] = 0;
    char *v = header(request, "x-bta-token");
    if (!v) return;
    unsigned n = 0;
    while (n < BTA_TOKEN_HEX && ((v[n] >= '0' && v[n] <= '9') || (v[n] >= 'a' && v[n] <= 'f'))) { out[n] = v[n]; n++; }
    out[n] = 0;
}
/* A LAN client with no (or a stale) token: tell it, in JSON, whether pairing is
 * even possible so the page can prompt for the code or ask the owner to turn
 * phone control on at the console. */
static void needs_pairing(struct app *a, struct client *c) {
    char body[48];
    struct writer w = {body, 0, sizeof(body)};
    append(&w, "{\"api\":");
    number(&w, BTA_API_VERSION);
    append(&w, ",\"paired\":false");
    field(&w, "lan", a->auth.enabled);
    append(&w, "}");
    response(c, "401 Unauthorized", "application/json", body, w.used);
}
static void dispatch(struct app *a, struct client *c) {
    char body[12000];
    if (!host_valid(c->input)) { response(c, "403 Forbidden", "text/plain", "Use the PS4 IP address.", 23); return; }
    /* The page itself carries no state and must load so a LAN client can see the
     * pairing prompt; everything under /api is gated. */
    if (starts(c->input, "GET / HTTP/1.")) { response_const(c, "200 OK", "text/html; charset=utf-8", web_page, sizeof(web_page) - 1); return; }
    char token_hdr[BTA_TOKEN_HEX + 1];
    read_token(c->input, token_hdr);
    int authed = bta_auth_request_ok(&a->auth, c->loopback, token_hdr);
    if (c->loopback) a->console_at = a->clock(); /* the console app is still there (conn_add_over) */
    /* Pairing: a LAN client posts the code shown on the TV and is handed a token.
     * Open to unpaired clients by design; still needs the anti-CSRF header. */
    if (starts(c->input, "POST /api/pair/")) {
        char *control = header(c->input, "x-bta-control");
        if (!control || !starts(control, "1\r\n")) { response(c, "403 Forbidden", "text/plain", "Control header required.", 24); return; }
        char code[BTA_CODE_DIGITS + 1];
        const char *p = c->input + 15;
        for (int i = 0; i < BTA_CODE_DIGITS; i++) {
            if (p[i] < '0' || p[i] > '9') { response(c, "400 Bad Request", "text/plain", "Enter the six-digit code.", 25); return; }
            code[i] = p[i];
        }
        code[BTA_CODE_DIGITS] = 0;
        if (!starts(p + BTA_CODE_DIGITS, " HTTP/1.")) { response(c, "400 Bad Request", "text/plain", "Enter the six-digit code.", 25); return; }
        u8 rnd[16];
        char token[BTA_TOKEN_HEX + 1], issued[BTA_TOKEN_HEX + 1];
        fill_random(a, rnd, sizeof(rnd));
        bta_auth_make_token(issued, rnd);
        int rc = bta_auth_pair(&a->auth, code, issued, token, a->clock(), PAIR_COOLDOWN_US);
        if (rc < 0) { response(c, "403 Forbidden", "text/plain", "Phone control is off or locked. Enable it on the console.", 57); return; }
        if (rc == 0) { response(c, "403 Forbidden", "text/plain", "Wrong code. Check the code on the TV.", 37); return; }
        struct writer w = {body, 0, sizeof(body)};
        append(&w, "{\"api\":");
        number(&w, BTA_API_VERSION);
        append(&w, ",\"paired\":true,\"token\":");
        quoted(&w, token);
        append(&w, "}");
        response(c, "200 OK", "application/json", body, w.used);
        return;
    }
    if (starts(c->input, "GET /api/status HTTP/1.")) { if (!authed) { needs_pairing(a, c); return; } unsigned n = status(a, body, sizeof(body), c->loopback); response(c, "200 OK", "application/json", body, n); return; }
    if (starts(c->input, "GET /api/timing HTTP/1.")) { if (!authed) { needs_pairing(a, c); return; } unsigned n = timing_json(a, body, sizeof(body)); response(c, "200 OK", "application/json", body, n); return; }
    char *control = header(c->input, "x-bta-control");
    if (!starts(c->input, "POST ") || !control || !starts(control, "1\r\n")) {
        response(c, "403 Forbidden", "text/plain", "Control header required.", 24);
        return;
    }
    if (!authed) { needs_pairing(a, c); return; }
    /* Changing who can reach the service is a console-only action: a paired
     * phone must not be able to widen access or lock the owner out. */
    if (starts(c->input, "POST /api/lan-on HTTP/1.") || starts(c->input, "POST /api/lan-off HTTP/1.")) {
        if (!c->loopback) { response(c, "403 Forbidden", "text/plain", "Change phone control at the console.", 36); return; }
        if (starts(c->input, "POST /api/lan-on HTTP/1.")) {
            if (put(LAN_ON, "on\n", 3)) { response(c, "500 Internal Server Error", "text/plain", "Cannot save preference.", 23); return; }
        } else if (exists(LAN_ON) && sc3(SYS_unlink, (long)LAN_ON, 0, 0) < 0) {
            response(c, "500 Internal Server Error", "text/plain", "Cannot save preference.", 23); return;
        }
        /* In-memory only here; network_tick persists it after the lock drops. */
        a->lan_act = sync_lan(a, exists(LAN_ON), a->lan_code);
        message(a, a->auth.enabled ? "Phone control is on. Enter the code shown on the TV." : "Phone control is off.");
    } else if (starts(c->input, "POST /api/scan HTTP/1.")) {
        /* Add Device: stop reconnecting, let go of the device, then search once
         * the radio is free (begin_add). One search at a time: each runs the radio
         * the controller shares for 16 s, so a repeat lets the running one finish. */
        if (ensure_bt(a)) { response(c, "503 Service Unavailable", "text/plain", "Bluetooth is not ready.", 23); return; }
        begin_add(a, c->loopback ? CONN_ADD_CONSOLE : CONN_ADD_PHONE);
    } else if (starts(c->input, "POST /api/scan-done HTTP/1.")) {
        end_add(a, "add: Add Device closed", 1);
    } else if (starts(c->input, "POST /api/select/")) {
        u64 mac = parse_mac(c->input + 17);
        if (!mac) { response(c, "400 Bad Request", "text/plain", "Use a six-byte MAC address.", 27); return; }
        refresh_devices(a);
        /* Any well-formed address is dialled: an odd class of device is no reason to
         * refuse, and one no list holds is taken at its word (note_device). */
        struct bt_device *d = note_device(a, mac, 0);
        struct bt_device chosen = {mac, 0, 0, 0, 0, {0}};
        if (d) chosen = *d;
        u64 old = a->target.address;
        a->v10_reset_pending = 0; /* user changed the recovery target */
        end_add(a, "add: ended, a device was chosen", 0);
        /* Already dialling this one: that attempt carries on. */
        if (!(old == mac && a->wanted && a->page_at)) {
            end_scan(a); stop_audio(a);
            if (old != mac) release_link(a); /* the old device's link or dial: the new dial waits for it */
            if (select_device(a, &chosen)) { response(c, "500 Internal Server Error", "text/plain", "Cannot save selection.", 22); return; }
            a->wanted = 1; a->state = CONN_RECOVERING; a->next_try = 0;
            a->dropped = a->give_up = a->announced = a->session = 0;
            a->attempts = 0; a->relinks = 0; a->refused_since = 0;
            sc3(SYS_unlink, (long)TV_ON, 0, 0); a->tv = 0;
            say(a, "Connecting to ", "...");
        }
    } else if (starts(c->input, "POST /api/pin/")) {
        char pin[17];
        unsigned n = 0;
        const char *p = c->input + 14;
        while (p[n] >= '0' && p[n] <= '9' && n < 16) { pin[n] = p[n]; n++; }
        if (!a->pin_pending || !n || !starts(p + n, " HTTP/1.")) { response(c, "400 Bad Request", "text/plain", "Invalid pairing PIN.", 20); return; }
        pin[n] = 0;
        struct { u64 address; char *pin; unsigned length, pad; } q = {a->target.address, pin, n, 0};
        long rc = sc3(SYS_ioctl, a->bt, BT_PIN_REPLY, (long)&q);
        if (rc) { response(c, "500 Internal Server Error", "text/plain", "PIN submission failed.", 22); return; }
        a->pin_pending = 0;
        a->state = CONN_CONNECTING; a->deadline = a->clock() + 30000000;
        message(a, "Pairing PIN submitted.");
    } else if (starts(c->input, "POST /api/volume/")) {
        const char *p = c->input + 17;
        unsigned value = 0, n = 0;
        while (p[n] >= '0' && p[n] <= '9' && n < 3) { value = value * 10 + (unsigned)(p[n] - '0'); n++; }
        if (!n || value > BTA_V11_MAX_VOLUME || !starts(p + n, " HTTP/1.")) {
            response(c, "400 Bad Request", "text/plain", "Volume must be 0-200.", 21); return;
        }
        if (save_volume(a, (int)value)) { response(c, "500 Internal Server Error", "text/plain", "Cannot save volume.", 19); return; }
        log_num(a, "FW14 V11 volume slider percent=", a->volume);
        log_num(a, "FW14 V11 volume post PCM gain percent=", bta_v11_gain_pct(a->volume, a->target.address == BTA_V8_FREEPODS_MAC));
    } else if (starts(c->input, "POST /api/test HTTP/1.")) {
        /* Streaming is the whole precondition: the tone goes out the send the audio
         * does. The link state is deliberately not re-read while streaming. */
        if (!a->streaming || a->bt < 0) { response(c, "409 Conflict", "text/plain", "Connect a device first.", 23); return; }
        a->test_phase = 0; bta_v12_tone_reset(&a->tone_clock);
        a->test_until = a->clock() + 2000000; a->snap = 1;
        a->diag6_test_prev = 1;
        log_num(a, "FW14 DIAG6 TEST begin stream id=", a->diag6_stream_id);
        message(a, "Playing test tone.");
    } else if (starts(c->input, "POST /api/connect HTTP/1.")) {
        end_add(a, "add: ended, connect", 0);
        a->wanted = 1; a->relinks = 0;
        sc3(SYS_unlink, (long)TV_ON, 0, 0); a->tv = 0;
        if (!a->streaming && !a->page_at) { /* a dial already running carries on */
            end_scan(a); a->state = CONN_RECOVERING; a->next_try = 0;
            a->dropped = a->give_up = a->announced = a->session = 0;
            a->attempts = 0;
            say(a, "Connecting to ", "...");
        }
    } else if (starts(c->input, "POST /api/recover/audio HTTP/1.") ||
               starts(c->input, "POST /api/recover/link HTTP/1.")) {
        /* Manual only. The sender already holds the service lock while handling
         * this request. No new kernel memory access or HCI command is introduced.
         * 'audio' keeps the link, 'link' follows the existing disconnect/radio
         * hold/reconnect path. Never mistake a driver return for audible sound. */
        u64 now = a->clock();
        if (!a->streaming || !a->target.address || !a->wanted) {
            response(c, "409 Conflict", "text/plain", "Connect and stream a headset first.", 36); return;
        }
        if (!bta_v10_reset_allowed(a->streaming, a->target.address, now, a->v10_reset_next_at)) {
            response(c, "429 Too Many Requests", "text/plain", "Recovery cooling down. Retry later.", 35); return;
        }
        int full = starts(c->input, "POST /api/recover/link HTTP/1.");
        a->v10_reset_count++;
        a->v10_reset_kind = full ? BTA_V10_RESET_LINK : BTA_V10_RESET_AUDIO;
        a->v10_reset_at = now;
        a->v10_reset_next_at = now + BTA_V10_RESET_COOLDOWN_US;
        a->v10_reset_pending = 1;
        a->v10_reset_stream_from = a->diag6_stream_id;
        log_num(a, "FW14 V10 RESET requested stream id=", a->v10_reset_stream_from);
        log_num(a, "FW14 V10 RESET previous BT_SEND_AUDIO rc=", a->last_rc);
        log_text(a, full ? "FW14 V10 RESET link: stopping audio, releasing link via existing radio hold" :
                            "FW14 V10 RESET audio: keeping Bluetooth link, refreshing stream");
        if (full) {
            end_add(a, "add: ended, manual recovery", 0);
            end_scan(a);
            stop_audio(a);
            release_link(a);
            a->wanted = 1; a->state = CONN_RECOVERING;
            a->next_try = now + 2000000;
            a->dropped = a->give_up = a->announced = a->session = 0;
            a->attempts = 0; a->refused_since = 0; a->relinks = 0;
            message(a, "Rebuilding the Bluetooth link. Verify sound after reconnect.");
        } else {
            restart_audio(a, "Restarting audio stream on the current Bluetooth link.");
        }
    } else if (starts(c->input, "POST /api/stop HTTP/1.")) {
        end_add(a, "add: ended, disconnect", 0);
        /* release_link holds the radio for a dial it cancels: a search right after
         * a Disconnect waits for that, too. */
        a->v10_reset_pending = 0; /* user chose ordinary Disconnect */
        a->wanted = 0; end_scan(a); stop_audio(a); release_link(a);
        a->state = CONN_IDLE; a->relinks = 0; a->refused_since = 0;
        closefd(a->bt); a->bt = -1;
        a->dropped = a->give_up = a->announced = a->session = 0;
        a->attempts = 0;
        message(a, "Audio stopped. TV sound restored.");
    } else if (starts(c->input, "POST /api/joint-on HTTP/1.") || starts(c->input, "POST /api/joint-off HTTP/1.")) {
        int joint = starts(c->input, "POST /api/joint-on HTTP/1.");
        if (!a->target.address) { response(c, "409 Conflict", "text/plain", "Select a device first.", 22); return; }
        bta_profile_touch(&a->profiles, &a->target);
        bta_profile_set_mode(&a->profiles, a->target.address, joint ? SBC_JOINT : SBC_DUAL);
        if (save_profiles(a)) { response(c, "500 Internal Server Error", "text/plain", "Cannot save preference.", 23); return; }
        a->mode = joint ? SBC_JOINT : SBC_DUAL;
        log_num(a, "FW14 V7 stereo toggle send type=", conn_send_type(a->mode));
        message(a, joint ? "Joint stereo is on for this device." : "Joint stereo is off for this device.");
    } else if (starts(c->input, "POST /api/tune/")) {
        const char *p = c->input + sizeof("POST /api/tune/") - 1;
        struct bta_v9_tune candidate = a->tune;
        int ok = 0;
        if (starts(p, "reset HTTP/1.")) {
            bta_v9_tune_init(&candidate, bta_pace_target(a->latency_mode) / 384u);
            ok = 1;
        } else {
            int knob = -1;
            if (starts(p, "cushion/")) { knob = BTA_V9_CUSHION; p += 8; }
            else if (starts(p, "margin/")) { knob = BTA_V9_MARGIN; p += 7; }
            else if (starts(p, "poll/")) { knob = BTA_V9_POLL; p += 5; }
            unsigned n = 0, value = 0;
            while (knob >= 0 && n < 4 && p[n] >= '0' && p[n] <= '9') {
                value = value * 10 + (unsigned)(p[n] - '0'); n++;
            }
            if (n > 0 && starts(p + n, " HTTP/1."))
                ok = bta_v9_tune_set(&candidate, knob, value);
        }
        if (!ok) { response(c, "400 Bad Request", "text/plain", "Invalid tuning value.", 21); return; }
        tune_apply(a, &candidate);
        char msg[128];
        struct writer mw = {msg, 0, sizeof(msg)};
        append(&mw, "FW14 V9 CHANGE cushion_ms="); number(&mw, a->tune.cushion_ms);
        append(&mw, " margin_ms="); number(&mw, a->tune.margin_ms);
        append(&mw, " poll_us="); number(&mw, a->tune.poll_us);
        log_text(a, msg);
        message(a, "Live audio tuning updated. No reconnect needed.");
    } else if (starts(c->input, "POST /api/latency/")) {
        const char *p = c->input + 18;
        if (p[0] < '0' || p[0] > '2' || !starts(p + 1, " HTTP/1.")) { response(c, "400 Bad Request", "text/plain", "Latency mode must be 0-2.", 25); return; }
        if (save_latency(a, p[0] - '0')) { response(c, "500 Internal Server Error", "text/plain", "Cannot save latency mode.", 25); return; }
        message(a, a->latency_mode == BTA_LAT_LOW ? "Latency mode: lowest." : a->latency_mode == BTA_LAT_STABLE ? "Latency mode: most stable." : "Latency mode: balanced.");
    } else if (starts(c->input, "POST /api/tv-on HTTP/1.")) {
        if (put(TV_ON, "on\n", 3)) { response(c, "500 Internal Server Error", "text/plain", "Cannot save preference.", 23); return; }
        a->tv = 1; mute(a, 0);
    } else if (starts(c->input, "POST /api/tv-off HTTP/1.")) {
        if (exists(TV_ON) && sc3(SYS_unlink, (long)TV_ON, 0, 0) < 0) { response(c, "500 Internal Server Error", "text/plain", "Cannot save preference.", 23); return; }
        a->tv = 0;
        if (a->streaming) mute(a, 1);
    } else if (starts(c->input, "POST /api/quit HTTP/1.")) {
        a->wanted = 0; stop_audio(a); a->quit = 1;
        a->state = CONN_IDLE; a->dropped = a->give_up = a->announced = a->session = 0;
        a->attempts = 0;
        message(a, "Bluetooth Audio stopped. TV sound restored.");
    } else {
        response(c, "404 Not Found", "text/plain", "Not found", 9);
        return;
    }
    unsigned n = status(a, body, sizeof(body), c->loopback);
    response(c, "200 OK", "application/json", body, n);
}
/* Log one line per notable request, never the routine polls (the app asks for the
 * status twice a second): failures, pairings, and https:// attempts the plain-HTTP
 * server can't answer. A path that carries a secret (the pairing code, a PIN) is
 * cut before it. Called with the lock held. */
static void log_request(struct app *a, struct client *c, const char *what) {
    char line[96], req[48];
    /* A phone tab left on the pairing screen polls twice a second and every reply
     * is a logged 401, which would churn the whole log in minutes. Same bound as
     * log_event: a burst is visible, a stuck client cannot push everything else out. */
    u64 now = a->clock();
    if (now >= a->http_at + 10000000) { a->http_at = now; a->http_lines = 0; }
    if (a->http_lines++ >= 12) return;
    struct writer w = {line, 0, sizeof(line)};
    append(&w, "http: ");
    if (what) append(&w, what);
    else {
        if (c->head < 12) return;
        const char *code = c->output + 9;
        int pair = starts(c->input, "POST /api/pair/");
        if (code[0] == '2' && !pair) return;
        unsigned i = 0;
        for (; c->input[i] && c->input[i] != '\r' && i < sizeof(req) - 1; i++)
            req[i] = c->input[i] >= 32 && c->input[i] < 127 ? c->input[i] : '?';
        req[i] = 0;
        char *p = find(req, " HTTP/");
        if (p) *p = 0;
        if ((p = find(req, "/api/pair/"))) p[9] = 0;
        if ((p = find(req, "/api/pin/"))) p[8] = 0;
        char status[4] = {code[0], code[1], code[2], 0};
        if (code[0] == '2') append(&w, "a phone paired");
        else { append(&w, status); append(&w, " "); append(&w, req); }
    }
    append(&w, c->loopback ? " (console)" : " (network)");
    log_text(a, line);
}
/* Rest mode aborts every socket. The aborted listener still completes a connection
 * and then resets it, so the app and the phone see the service as gone while audio
 * plays on. Open a fresh one (at most every 2 s) and drop the client connections,
 * which are dead too. `why` is the accept error, or 0 after a wake or a retry. */
static void reopen_listener(struct app *a, u64 now, long why) {
    for (int i = 0; i < 4; i++) { closefd(a->clients[i].fd); a->clients[i].fd = -1; }
    closefd(a->listener);
    a->listener = bta_listen();
    a->listen_at = now + 2000000;
    a->accept_errs = 0;
    lock_state(a);
    log_num(a, a->listener >= 0 ? "http: listening again; cause " : "http: port 8097 still busy; cause ", why);
    unlock_state(a);
}
static void network_tick(struct app *a, u64 now) {
    if (a->listener < 0 && now >= a->listen_at) reopen_listener(a, now, 0);
    for (int i = 0; i < 4; i++) {
        struct client *c = &a->clients[i];
        if (c->fd < 0) {
            if (a->listener < 0) continue;
            struct sockaddr4 peer;
            u32 len = sizeof(peer);
            long fd = sc3(SYS_accept, a->listener, (long)&peer, (long)&len);
            if (fd < 0) {
                /* -35: nobody is waiting. Other errors in a row mean the listening
                 * socket itself is broken. */
                if (fd != -35 && ++a->accept_errs >= 3 && now >= a->listen_at) reopen_listener(a, now, fd);
                continue;
            }
            a->accept_errs = 0;
            if (len != 16 || !local_ip((u8 *)&peer.addr) || nonblock(fd) < 0) { closefd(fd); continue; }
            int one = 1;
            sc5(SYS_setsockopt, fd, 0xffff, 0x800, (long)&one, 4);
            c->fd = fd; c->used = c->sent = c->head = c->total = 0; c->tail = 0; c->input[0] = 0; c->deadline = now + 3000000;
            c->loopback = (((u8 *)&peer.addr)[0] == 127); /* the console reaches us here */
        }
        if (now > c->deadline) { closefd(c->fd); c->fd = -1; continue; }
        if (!c->total) {
            long n = sc3(SYS_read, c->fd, (long)(c->input + c->used), sizeof(c->input) - c->used - 1);
            if (n == 0 || (n < 0 && n != -35)) { closefd(c->fd); c->fd = -1; continue; }
            if (n > 0) {
                c->used += (unsigned)n;
                c->input[c->used] = 0;
                /* 0x16 opens a TLS handshake: the browser asked for https://, which
                 * this plain-HTTP server can't answer. Close, and say so in the log. */
                if ((u8)c->input[0] == 0x16) {
                    lock_state(a); log_request(a, c, "an https:// request; the page is http:// only"); unlock_state(a);
                    closefd(c->fd); c->fd = -1; continue;
                }
                if (find(c->input, "\r\n\r\n")) {
                    lock_state(a); dispatch(a, c); log_request(a, c, 0); unlock_state(a);
                    /* A phone-control toggle leaves its file write and TV notice
                     * here, outside the lock, so flash I/O can't stall the sender. */
                    if (a->lan_act) { persist_lan(a, a->lan_act, a->lan_code); a->lan_act = 0; }
                }
                else if (c->used == sizeof(c->input) - 1) response(c, "431 Request Header Fields Too Large", "text/plain", "Request too large", 17);
            }
        }
        if (c->total) {
            int in_head = c->sent < c->head;
            const char *from = in_head ? c->output + c->sent : c->tail + (c->sent - c->head);
            long n = sc3(SYS_write, c->fd, (long)from, (in_head ? c->head : c->total) - c->sent);
            if (n > 0) c->sent += (unsigned)n;
            if (c->sent == c->total || (n < 0 && n != -35)) { closefd(c->fd); c->fd = -1; }
        }
    }
}
/* Feed the driver the way the PS4's own audio daemon does. orbis_audiod creates
 * its Bluetooth senders as dedicated threads with explicit scheduling:
 * SCHED_FIFO, priority 0x132 (306) and CPU mask 0xc0 (cores 6-7), away from game
 * threads. Our sender does nothing but capture ring -> driver; the web page,
 * Bluetooth management and status file stay on the worker thread. One mutex
 * guards the shared state. If the console refuses the scheduling attributes, the
 * sender runs with defaults; if no thread can be created, the worker loop sends
 * audio itself, as before. */
static void init_lock(struct app *a) {
    /* Priority inheritance: while the sender waits for the lock, its holder runs
     * at the sender's priority, so a paused housekeeping thread cannot stall the
     * audio. */
    int (*init)(void **, void **, const char *) = 0, (*ainit)(void **) = 0, (*proto)(void **, int) = 0, (*adestroy)(void **) = 0;
    void *attr = 0;
    void **ap = 0;
    a->pi_rc = -1;
    if (!resolve("scePthreadMutexattrInit", &ainit) && ainit && !ainit(&attr)) {
        ap = &attr;
        if (!resolve("scePthreadMutexattrSetprotocol", &proto) && proto) a->pi_rc = proto(ap, 1);
    }
    if (resolve("scePthreadMutexInit", &init) || resolve("scePthreadMutexLock", &a->mlock) || resolve("scePthreadMutexUnlock", &a->munlock) || !init || !a->mlock || !a->munlock)
        a->mlock = a->munlock = 0;
    else if (init(&a->mutex, ap, "Bluetooth Audio state")) {
        a->pi_rc = -2;
        if (init(&a->mutex, 0, "Bluetooth Audio state")) a->mlock = a->munlock = 0;
    }
    if (ap && !resolve("scePthreadMutexattrDestroy", &adestroy) && adestroy) adestroy(ap);
}
static void *sender(void *arg) {
    struct app *a = arg;
    void *(*self)(void) = 0;
    int (*getprio)(void *, int *) = 0;
    if (!resolve("scePthreadSelf", &self) && !resolve("scePthreadGetprio", &getprio) && self && getprio) {
        int p = 0;
        if (!getprio(self(), &p)) a->sender_prio = p;
    }
    u64 sleep_start = 0;
    u32 last_poll = 0;
    while (a->sender_run) {
        u64 lock_start = a->clock();
        lock_state(a);
        /* Fix V12's stale-time bug: pacing uses time AFTER any mutex wait. */
        u64 now = a->clock();
        bta_v13_note(&a->sender_diag, lock_start, now, sleep_start, last_poll);
        audio_tick(a, now);
        u32 poll_us = a->tune.poll_us;
        unlock_state(a);
        sleep_start = a->clock();
        last_poll = poll_us;
        a->sleep(poll_us);
    }
    a->sender_alive = 0;
    return 0;
}
static void start_sender(struct app *a) {
    if (!a->mlock) return; /* no lock, no second thread */
    int (*create)(void **, void **, void *(*)(void *), void *, const char *) = 0;
    int (*detach)(void *) = 0;
    if (resolve("scePthreadCreate", &create) || resolve("scePthreadDetach", &detach) || !create || !detach) { a->thread_rc = -1; return; }
    int (*init)(void **) = 0, (*inherit)(void **, int) = 0, (*policy)(void **, int) = 0, (*param)(void **, const int *) = 0, (*affinity)(void **, u64) = 0, (*destroy)(void **) = 0;
    void *attr = 0;
    void **ap = 0;
    if (!resolve("scePthreadAttrInit", &init) && init && !init(&attr)) {
        ap = &attr;
        int prio = 0x132;
        if (!resolve("scePthreadAttrSetinheritsched", &inherit) && inherit) inherit(ap, 0);
        if (!resolve("scePthreadAttrSetschedpolicy", &policy) && policy) policy(ap, 1);
        a->prio_rc = (!resolve("scePthreadAttrSetschedparam", &param) && param) ? param(ap, &prio) : -1;
        a->aff_rc = (!resolve("scePthreadAttrSetaffinity", &affinity) && affinity) ? affinity(ap, 0xc0) : -1;
    }
    void *thread = 0;
    a->sender_run = 1;
    a->sender_alive = 1;
    a->thread_rc = create(&thread, ap, sender, a, "Bluetooth Audio sender");
    if (a->thread_rc && ap) {
        a->prio_rc = a->aff_rc = -2;
        a->thread_rc = create(&thread, 0, sender, a, "Bluetooth Audio sender");
    }
    if (a->thread_rc) { a->sender_run = 0; a->sender_alive = 0; } else detach(thread);
    if (ap && !resolve("scePthreadAttrDestroy", &destroy) && destroy) destroy(ap);
}
/* The sender uses this stack frame's state: wait for it to leave before returning. */
static void stop_sender(struct app *a) { a->sender_run = 0; while (a->sender_alive) a->sleep(1000); }

void *bta_worker(void *arg) {
    struct app a;
    memset(&a, 0, sizeof(a));
    a.listener = (long)arg;
    a.bt = a.hdmi = a.shmfd = -1;
    a.muted = -1;
    for (int i = 0; i < 4; i++) a.clients[i].fd = -1;
    if (resolve("sceKernelUsleep", &a.sleep) || resolve("sceKernelGetProcessTime", &a.clock) || resolve("shm_open", &a.shm) || resolve("mmap", &a.map) || resolve("munmap", &a.unmap) || !a.sleep || !a.clock || !a.shm || !a.map || !a.unmap) {
        closefd(a.listener);
        return 0;
    }
    sc3(SYS_mkdir, (long)"/data/bta_audio", 0777, 0);
    sc3(SYS_chmod, (long)"/data/bta_audio", 0777, 0);
    sc3(SYS_rename, (long)SERVICE_LOG, (long)SERVICE_LOG_OLD, 0);
    a.log_t0 = a.clock();
    { char m[64], d[11];
      u64 w = wall_us();
      date_text(w / 1000000, d);
      compose(m, sizeof(m), "Bluetooth Audio service 3.0.0 started ", w ? d : "(no clock)", ", times UTC");
      log_text(&a, m); }
    /* Resolve the kernel routines by signature, once, before anything asks for a
     * capability. Its report line is what a user on an untested firmware quotes in a
     * bug report, so it reaches the file at once: should the kernel work later kill
     * the console, that line must survive it. */
    bta_kernel_init();
    log_text(&a, BTA_V8_PACE_CATCHUP ?
        "FW14 V8 build=GAIN-CATCHUP (SpaceBuds HCI enabled; pacing experiment)" :
        "FW14 V8 build=GAIN-DIAG (SpaceBuds HCI enabled; V7 pacing preserved)");
    log_num(&a, "FW14 V8 FreePods gain percent=", BTA_V8_FREEPODS_GAIN_PCT);
    log_text(&a, "FW14 V10 build=MANUAL-RECOVERY (V9 tuner, V8 gain, V7 packet slots; no automatic reset)");
    log_text(&a, "FW14 V11 build=VOLUME-BOOST (0-200 percent, per-packet peak limiter, V10 recovery)");
    log_text(&a, "FW14 V12 build=TONE-CLOCK (V11 unchanged; fixed 4ms tone schedule, no bursts)");
    log_text(&a, "FW14 V13 build=LOCK-TIMESTAMP (V12 tone clock, post-lock timestamp, wait diagnostics)");
    log_text(&a, "FW14 V14 build=HDMI-UNLOCK (V13 sender diagnostics; HDMI reassert outside shared audio mutex)");
    bta_kernel_report(a.kernel, sizeof(a.kernel)); log_text(&a, a.kernel);
    write_log(&a, a.log, a.log_len); a.log_len = 0; /* no sender thread yet: no lock needed */
    sc3(SYS_unlink, (long)BTA_STOP, 0, 0);
    resolve("sceKernelGetRandomNumber", &a.getrandom); /* optional; fill_random falls back */
    load_profiles(&a);
    init_target(&a);
    load_volume(&a);
    bta_v9_tune_init(&a.tune, 12);
    load_latency(&a);
    a.tv = exists(TV_ON);
    sc3(SYS_unlink, (long)PAIR_CODE, 0, 0); /* a code never outlives the service session */
    { char code[BTA_CODE_DIGITS + 1]; /* phone control stays off unless the flag file is present */
      persist_lan(&a, sync_lan(&a, exists(LAN_ON), code), code); }
    a.hdmi = sc3(SYS_open, (long)"/dev/hdmi", 2, 0);
    if (a.wanted) {
        /* A remembered device exists: reconnect to whichever one is turned on. */
        a.state = CONN_RECOVERING; a.next_try = 0;
        say(&a, "Reconnecting to ", "...");
        notify("Bluetooth Audio is reconnecting to your last audio device. Open the app to change the device or settings.");
    } else {
        message(&a, "Ready. Select an audio device to connect.");
        notify("Bluetooth Audio is ready. Open the app to connect an audio device. Web controls: http://127.0.0.1:8097/ (or your PS4 IP on port 8097).");
    }
    init_lock(&a);
    start_sender(&a);
    while (!a.quit) {
        u64 now = a.clock(), wall = wall_us();
        /* Rest-mode resume (see conn_slept): the driver handle and the sockets may
         * be stale (the in-RAM patch is not — rest mode is not a reboot). Tear down,
         * drop any half-done connect (it may have started as the console went to
         * sleep), reopen the listener and let the normal connect path (which reopens
         * /dev/bt) reconnect from scratch. The process-clock check alone skips the
         * case where audio is still flowing (a recently accepted packet): then the
         * gap was only this normal-priority worker thread being starved while the
         * real-time sender kept streaming, not a real suspend. */
        u64 slept = a.last_wall && wall > a.last_wall ? wall - a.last_wall : 0;
        if (conn_slept(now - a.last_tick, slept, CONN_RESUME_GAP_US) ||
            (conn_resumed(a.last_tick, now, CONN_RESUME_GAP_US) &&
             !(a.streaming && now >= a.ok_at && now - a.ok_at < CONN_RESUME_GAP_US))) {
            lock_state(&a);
            log_num(&a, "wake: seconds asleep ", (long)(slept / 1000000));
            stop_audio(&a);
            closefd(a.bt); a.bt = -1;
            a.own_link = 0; a.redial_at = 0; /* a link that outlived the sleep is not trusted: dial afresh */
            /* No dial or search outlived it either; the radio rests before the next. */
            a.page_at = 0; a.hold = CONN_HOLD_NONE; a.scanning = 0; a.scan_wait = 0;
            a.radio_quiet_at = now + CONN_SCAN_QUIET_US;
            if (a.wanted) {
                a.state = CONN_RECOVERING; a.dropped = a.give_up = 0; a.attempts = 0;
                a.next_try = now + 2000000; /* let the console's own Bluetooth come back first */
                message(&a, "Woke from rest mode. Reconnecting...");
            } else a.state = CONN_IDLE;
            unlock_state(&a);
            reopen_listener(&a, now, 0);
        }
        a.last_tick = now;
        a.last_wall = wall;
        if (now >= a.net_at) { network_tick(&a, now); a.net_at = now + 4000; } /* locks around each request */
        if (a.quit) break;
        char json[12000], logbuf[sizeof(a.log)];
        unsigned n = 0;
        u32 logn = 0;
        int beat = now >= a.heartbeat;
        /* Snapshot keepalive decision while under the lock; HDMI fd stays open
         * until after stop_sender, so the call can execute safely outside. */
        long mute_fd = -1;
        /* Take the lock only when there is shared-state work (~35 times a second). */
        if (now < a.poll_at && a.sender_alive && now < a.meter_at && !beat) { a.sleep(500); continue; }
        lock_state(&a);
        if (now >= a.poll_at) { bluetooth_tick(&a, now); fw14_slots_tick(&a, now); a.poll_at = now + 50000; }
        if (!a.sender_alive) audio_tick(&a, now); /* fallback: no sender thread */
        /* Output meter: peak of the last ~80 ms, with a quick decay so a client
         * that polls a few times a second still sees movement. */
        if (now >= a.meter_at) {
            unsigned p = a.meter_peak * 100 / 32767;
            if (p > 100) p = 100;
            unsigned hold = (unsigned)a.meter * 7 / 10;
            a.meter = (int)(p > hold ? p : hold);
            a.meter_peak = 0;
            a.meter_at = now + 80000;
        }
        if (beat) {
            /* status.json is only for reading over FTP (clients use /api/status):
             * every 10 s spares the disk a write a second. reveal=0: the code
             * never lands in it. */
            if (now >= a.status_at) { n = status(&a, json, sizeof(json), 0); a.status_at = now + 10000000; }
            if (a.streaming && a.diag6_test_prev && now >= a.test_until) {
                log_num(&a, "FW14 DIAG6 TEST end stream id=", a.diag6_stream_id);
                v12_tone_emit(&a);
                a.diag6_test_prev = 0;
            }
            if (a.streaming && now >= a.diag6_at) {
                diag6_emit(&a);
                v13_sender_emit(&a);
                v14_hdmi_emit(&a);
                v8_emit(&a);
                a.diag6_at = now + 5000000;
            }
            if (a.streaming && now >= a.stats_at) { log_stats(&a, "audio: "); a.stats_at = now + 600000000; }
            /* The PS4 sets HDMI up again, which clears its mute, whenever a game or the
             * home screen comes to the front (KLog: "Hdmi Setup" on each switch), and
             * mute() only acts on a change it knows about. Put the mute back each
             * second while it should hold. */
            if (bta_v14_mute_needed(a.streaming, a.muted, a.hdmi)) mute_fd = a.hdmi;
            a.level = 0; a.heartbeat = now + 1000000; logn = take_log(&a, logbuf);
        }
        unlock_state(&a);
        if (beat) {
            /* Reassert HDMI mute once per second OUTSIDE the audio-state mutex.
             * This ioctl previously monopolized the sender lock, potentially for
             * ~8ms; the sender must not wait for HDMI driver latency. Connection
             * and mute transitions still use the existing guarded mute(). */
            if (mute_fd >= 0) {
                int v = 1;
                u64 call_start = a.clock();
                long rc = sc3(SYS_ioctl, mute_fd, HDMI_SET_MUTE, (long)&v);
                u64 call_end = a.clock();
                lock_state(&a);
                a.hdmi_rc = rc;
                bta_v14_hdmi_note(&a.hdmi_diag, call_start, call_end, rc);
                unlock_state(&a);
            }
            /* File I/O stays outside the lock: a slow write must never hold up the sender. */
            if (n) put("/data/bta_audio/status.json", json, n);
            write_log(&a, logbuf, logn);
            if (a.profile_dirty) {
                lock_state(&a);
                struct bta_profiles snap = a.profiles; a.profile_dirty = 0;
                unlock_state(&a);
                put_profiles(&snap);
            }
            if (exists(BTA_STOP)) break;
            int tv = exists(TV_ON);
            int lan = exists(LAN_ON);
            char lancode[BTA_CODE_DIGITS + 1];
            int lanact;
            lock_state(&a);
            a.tv = tv;
            lanact = sync_lan(&a, lan, lancode); /* owner may toggle phone control via the flag file or the app */
            unlock_state(&a);
            persist_lan(&a, lanact, lancode); /* file write and TV notice stay outside the lock */
            /* The address can arrive late (network still coming up) or change (DHCP). */
            if (a.auth.enabled && now >= a.ip_at) { a.ip = lan_address(); a.ip_at = now + (a.ip ? 30000000 : 5000000); }
            /* Disconnect is detected by the audio send failing (see audio_tick),
             * as the proven bridge did. No per-second driver state poll while streaming. */
        }
        a.sleep(500);
    }
    stop_sender(&a);
    end_scan(&a);
    stop_audio(&a);
    drop_link(&a); /* leave no idle link behind: the next run would find it stale (see conn_plan_link) */
    a.wanted = 0;
    message(&a, "Bluetooth Audio is offline. TV sound restored.");
    char json[12000];
    unsigned n = status(&a, json, sizeof(json), 0);
    put("/data/bta_audio/status.json", json, n);
    write_log(&a, a.log, a.log_len); /* the sender has stopped: no lock, no copy */
    for (int i = 0; i < 4; i++) closefd(a.clients[i].fd);
    closefd(a.listener);
    closefd(a.bt);
    closefd(a.hdmi);
    return 0;
}
