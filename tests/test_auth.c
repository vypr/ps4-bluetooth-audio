#include "test.h"
#include "auth.h"
#include <string.h>

/* Access control for the control API: the console (loopback) always has full
 * access; a LAN client can read or change nothing until it pairs with the
 * six-digit code and uses the token it is given. */
void test_auth(void) {
    /* Code formatting: fixed six digits, wrapped modulo 1,000,000. */
    { char c[BTA_CODE_DIGITS + 1]; u8 r[4] = {0, 0, 0, 0}; bta_auth_make_code(c, r);
      CHECK(!strcmp(c, "000000")); }
    { char c[BTA_CODE_DIGITS + 1]; u8 r[4] = {0x40, 0x42, 0x0f, 0}; bta_auth_make_code(c, r); /* 0x0f4240 = 1,000,000 */
      CHECK(!strcmp(c, "000000")); }
    { char c[BTA_CODE_DIGITS + 1]; u8 r[4] = {0x39, 0x30, 0, 0}; bta_auth_make_code(c, r); /* 0x3039 = 12345 */
      CHECK(!strcmp(c, "012345")); CHECK_EQ(strlen(c), BTA_CODE_DIGITS); }

    /* Token formatting: 32 lowercase hex digits. */
    { char t[BTA_TOKEN_HEX + 1]; u8 r[16]; for (int i = 0; i < 16; i++) r[i] = (u8)(i * 17);
      bta_auth_make_token(t, r);
      CHECK_EQ(strlen(t), BTA_TOKEN_HEX);
      CHECK(!strcmp(t, "00112233445566778899aabbccddeeff")); }

    /* Constant-time-style compare matches strcmp's equality result. */
    CHECK(bta_auth_same("123456", "123456"));
    CHECK(!bta_auth_same("123456", "123457"));
    CHECK(!bta_auth_same("123456", "12345"));   /* prefix is not a match */
    CHECK(!bta_auth_same("12345", "123456"));

    /* Loopback always passes, regardless of phone-control state or token. */
    { struct bta_auth s; bta_auth_disable(&s);
      CHECK(bta_auth_request_ok(&s, 1, 0));       /* console, phone control off */
      CHECK(!bta_auth_request_ok(&s, 0, 0));      /* LAN, phone control off: denied */
      CHECK(!bta_auth_request_ok(&s, 0, "anything")); }

    /* A full enable -> pair -> use -> disable cycle. */
    { struct bta_auth s;
      u8 codernd[4] = {0x39, 0x30, 0, 0};         /* -> code "012345" */
      bta_auth_enable(&s, codernd);
      CHECK(s.enabled);
      CHECK(!strcmp(s.code, "012345"));
      /* A LAN client with no token is still denied until it pairs. */
      CHECK(!bta_auth_request_ok(&s, 0, 0));

      char tok[BTA_TOKEN_HEX + 1], issued[BTA_TOKEN_HEX + 1];
      u8 tokrnd[16]; for (int i = 0; i < 16; i++) tokrnd[i] = (u8)(0xA0 + i);
      bta_auth_make_token(issued, tokrnd);

      /* Wrong code: rejected and counted, no token. */
      CHECK_EQ(bta_auth_pair(&s, "999999", issued, tok, 1000, 30000), 0);
      CHECK_EQ(s.pair_fails, 1u);
      CHECK_EQ(tok[0], 0);
      CHECK(!bta_auth_request_ok(&s, 0, issued));

      /* Right code: paired, token returned and now accepted. */
      CHECK_EQ(bta_auth_pair(&s, "012345", issued, tok, 1000, 30000), 1);
      CHECK(!strcmp(tok, issued));
      CHECK_EQ(s.pair_fails, 0u);                 /* success resets the counter */
      CHECK_EQ(s.token_count, 1u);
      CHECK(bta_auth_request_ok(&s, 0, issued));
      CHECK(!bta_auth_request_ok(&s, 0, "deadbeef")); /* a made-up token still fails */

      /* Disabling clears code and tokens; the old token stops working. */
      bta_auth_disable(&s);
      CHECK(!s.enabled);
      CHECK_EQ(s.token_count, 0u);
      CHECK(!bta_auth_request_ok(&s, 0, issued));
      CHECK(bta_auth_request_ok(&s, 1, 0));       /* console unaffected */
    }

    /* Cooldown after too many wrong codes, which then expires on its own. */
    { struct bta_auth s; u8 r[4] = {1, 0, 0, 0}; bta_auth_enable(&s, r);
      char tok[BTA_TOKEN_HEX + 1], issued[BTA_TOKEN_HEX + 1];
      u8 tr[16]; for (int i = 0; i < 16; i++) tr[i] = (u8)i; bta_auth_make_token(issued, tr);
      u64 cd = 30000;
      for (unsigned i = 0; i < BTA_MAX_PAIR_FAILS; i++) CHECK_EQ(bta_auth_pair(&s, "000000", issued, tok, 100, cd), 0);
      CHECK(s.lock_until == 100 + cd);
      CHECK_EQ(bta_auth_pair(&s, "000000", issued, tok, 200, cd), -1);   /* during cooldown */
      CHECK_EQ(bta_auth_pair(&s, s.code, issued, tok, 200, cd), -1);     /* even a right code waits */
      /* Once the cooldown elapses, the correct code pairs again (no permanent lockout). */
      CHECK_EQ(bta_auth_pair(&s, s.code, issued, tok, 100 + cd + 1, cd), 1);
      CHECK_EQ(s.pair_fails, 0u);
      CHECK(s.lock_until == 0); }

    /* Token table is a ring: the oldest is evicted once full. */
    { struct bta_auth s; u8 r[4] = {2, 0, 0, 0}; bta_auth_enable(&s, r);
      char first[BTA_TOKEN_HEX + 1] = {0}, tok[BTA_TOKEN_HEX + 1];
      for (unsigned i = 0; i < BTA_MAX_TOKENS + 1; i++) {
          char issued[BTA_TOKEN_HEX + 1]; u8 tr[16];
          for (int k = 0; k < 16; k++) tr[k] = (u8)(i * 31 + k);
          bta_auth_make_token(issued, tr);
          CHECK_EQ(bta_auth_pair(&s, s.code, issued, tok, 100, 30000), 1);
          if (i == 0) strcpy(first, issued);
      }
      CHECK_EQ(s.token_count, (unsigned)BTA_MAX_TOKENS);
      CHECK(!bta_auth_token_valid(&s, first));    /* the very first token was evicted */
      CHECK(bta_auth_token_valid(&s, tok)); }     /* the most recent still works */
}
