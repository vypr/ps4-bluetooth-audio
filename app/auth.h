#ifndef BTA_AUTH_H
#define BTA_AUTH_H
#include "bta_types.h"
/* Control-API access control for LAN clients.
 *
 * The console itself (the home-screen app, reaching the service over the
 * loopback address) always has full access: it is physically the owner's
 * console. Any other client on the local network — a phone or a PC browser —
 * is a LAN client and, by default, can read nothing and change nothing.
 *
 * To let a LAN client in, the owner turns phone control on at the console. The
 * service then shows a six-digit pairing code on the TV. A LAN client that
 * posts the matching code is issued an opaque token and sends it on every later
 * request. Turning phone control off clears the code and every issued token.
 *
 * This header is pure: all randomness is passed in by the caller, so the
 * decision logic and the code/token formatting are unit-tested on a PC. It
 * keeps no platform dependency (not even memset) so it builds in both the
 * payload and the host test. */

#define BTA_CODE_DIGITS    6        /* pairing code shown on the TV */
#define BTA_TOKEN_HEX      32        /* 128-bit token as lowercase hex */
#define BTA_MAX_TOKENS     4         /* at most this many paired LAN clients */
#define BTA_MAX_PAIR_FAILS 10        /* wrong codes before a pairing cooldown kicks in */

struct bta_auth {
    int enabled;                                   /* phone control on? */
    char code[BTA_CODE_DIGITS + 1];                /* current code, valid while enabled */
    char tokens[BTA_MAX_TOKENS][BTA_TOKEN_HEX + 1];/* issued tokens (in-memory only) */
    unsigned token_count;
    unsigned pair_fails;                           /* wrong-code attempts in this window */
    u64 lock_until;                                /* no pairing until this time (clock units) */
};

static void bta_auth_zero(struct bta_auth *s) {
    char *p = (char *)s;
    for (u64 i = 0; i < sizeof(*s); i++) p[i] = 0;
}

/* Six decimal digits (000000..999999) from four random bytes. */
static void bta_auth_make_code(char out[BTA_CODE_DIGITS + 1], const u8 rnd[4]) {
    u32 v = (u32)rnd[0] | ((u32)rnd[1] << 8) | ((u32)rnd[2] << 16) | ((u32)rnd[3] << 24);
    v %= 1000000u;
    for (int i = BTA_CODE_DIGITS - 1; i >= 0; i--) { out[i] = (char)('0' + v % 10); v /= 10; }
    out[BTA_CODE_DIGITS] = 0;
}

/* 32 lowercase hex digits from sixteen random bytes. */
static void bta_auth_make_token(char out[BTA_TOKEN_HEX + 1], const u8 rnd[16]) {
    const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) { out[2 * i] = hex[rnd[i] >> 4]; out[2 * i + 1] = hex[rnd[i] & 15]; }
    out[BTA_TOKEN_HEX] = 0;
}

/* Length-checked compare that does not short-circuit on the first differing
 * byte, so a caller cannot learn the match position from how long it took.
 * Both arguments are NUL-terminated. */
static int bta_auth_same(const char *a, const char *b) {
    u64 i = 0, diff = 0;
    for (; a[i] && b[i]; i++) diff |= (u64)((u8)a[i] ^ (u8)b[i]);
    diff |= (u64)((u8)a[i] ^ (u8)b[i]); /* both strings must end at the same point */
    return diff == 0;
}

/* Turn phone control off: forget the code and every token. */
static void bta_auth_disable(struct bta_auth *s) { bta_auth_zero(s); }

/* Turn phone control on with a fresh code; clears any existing tokens. */
static void bta_auth_enable(struct bta_auth *s, const u8 rnd[4]) {
    bta_auth_zero(s);
    s->enabled = 1;
    bta_auth_make_code(s->code, rnd);
}

/* True if `token` is one this auth has issued and still holds. */
static int bta_auth_token_valid(const struct bta_auth *s, const char *token) {
    if (!s->enabled || !token || !token[0]) return 0;
    int ok = 0;
    for (unsigned i = 0; i < s->token_count; i++)
        if (bta_auth_same(s->tokens[i], token)) ok = 1; /* no early return: constant work */
    return ok;
}

/* A LAN client posts a code. On a match the caller-supplied `newtoken` (filled
 * from random bytes) is stored and copied to `out`. `now` is the current time
 * and `cooldown` the pause imposed after too many wrong codes (same units).
 * Returns:
 *    1  paired; out holds the token
 *    0  wrong code; counted, and a cooldown starts after BTA_MAX_PAIR_FAILS
 *   -1  pairing unavailable (phone control off, or in a cooldown)
 *
 * The cooldown rate-limits a brute force (ten tries then a pause makes guessing
 * a six-digit code hopeless) but always expires on its own, so a LAN attacker
 * spamming wrong codes cannot lock the owner out for good. */
static int bta_auth_pair(struct bta_auth *s, const char *code,
                         const char newtoken[BTA_TOKEN_HEX + 1], char out[BTA_TOKEN_HEX + 1],
                         u64 now, u64 cooldown) {
    out[0] = 0;
    if (!s->enabled) return -1;
    if (s->lock_until) {
        if (now < s->lock_until) return -1;   /* still cooling down */
        s->lock_until = 0; s->pair_fails = 0; /* window elapsed: start fresh */
    }
    if (!code || !bta_auth_same(s->code, code)) {
        if (++s->pair_fails >= BTA_MAX_PAIR_FAILS) s->lock_until = now + cooldown;
        return 0;
    }
    s->pair_fails = 0; s->lock_until = 0;
    unsigned slot;
    if (s->token_count < BTA_MAX_TOKENS) {
        slot = s->token_count++;
    } else {
        /* Full: evict the oldest, keeping the newest BTA_MAX_TOKENS-1. */
        for (unsigned i = 1; i < BTA_MAX_TOKENS; i++)
            for (int k = 0; k <= BTA_TOKEN_HEX; k++) s->tokens[i - 1][k] = s->tokens[i][k];
        slot = BTA_MAX_TOKENS - 1;
    }
    for (int k = 0; k <= BTA_TOKEN_HEX; k++) s->tokens[slot][k] = newtoken[k];
    out[0] = 0;
    for (int k = 0; k <= BTA_TOKEN_HEX; k++) out[k] = newtoken[k];
    return 1;
}

/* The master decision: may this request read status or run a control action?
 * The console (loopback) always may; a LAN client needs a valid token. */
static int bta_auth_request_ok(const struct bta_auth *s, int loopback, const char *token) {
    if (loopback) return 1;
    return bta_auth_token_valid(s, token);
}
#endif
