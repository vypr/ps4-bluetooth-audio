#include "test.h"
#include "strfmt.h"
#include <string.h>

void test_strfmt(void) {
    char buf[128];

    /* compose: prefix + name + suffix, NUL-terminated, returns length. */
    { u64 n = compose(buf, sizeof buf, "Connecting to ", "JBL Go", "...");
      CHECK(!strcmp(buf, "Connecting to JBL Go..."));
      CHECK_EQ(n, (long)strlen("Connecting to JBL Go...")); }

    /* compose clips the device name to 40 bytes. */
    { char name[64]; memset(name, 'A', sizeof name); name[63] = 0;
      compose(buf, sizeof buf, "<", name, ">");
      CHECK_EQ(strlen(buf), 1 + 40 + 1); }

    /* compose respects the output capacity and still NUL-terminates. */
    { char small[8]; u64 n = compose(small, sizeof small, "abcdef", "ghij", "kl");
      CHECK(n <= 7);
      CHECK(small[n] == 0);
      CHECK(small[7] == 0 || n < 7); }

    /* mac_text: 48-bit address to 12 uppercase hex digits. */
    { char mac[13]; mac_text(0x0011223344AAUL, mac); CHECK(!strcmp(mac, "0011223344AA")); }

    /* parse_mac: valid, lowercase, bad hex, and the mandatory " HTTP/1." tail. */
    CHECK_EQ((long)parse_mac("0011223344AA HTTP/1.1"), (long)0x0011223344AAUL);
    CHECK_EQ((long)parse_mac("aabbccddeeff HTTP/1.0"), (long)0xAABBCCDDEEFFUL);
    CHECK_EQ((long)parse_mac("00112233445Z HTTP/1.1"), 0);     /* bad hex digit */
    CHECK_EQ((long)parse_mac("0011223344AA GET /"), 0);        /* missing HTTP tail */
    CHECK_EQ((long)parse_mac("0011223344AA"), 0);              /* truncated */

    /* joint_path: the per-device opt-out flag file path. */
    { char p[40]; joint_path(0x0011223344AAUL, p);
      CHECK(!strcmp(p, "/data/bta_audio/dual-0011223344AA")); }

    /* bta_prefix. */
    CHECK(bta_prefix("hello world", "hello"));
    CHECK(!bta_prefix("hello", "hello world"));
    CHECK(bta_prefix("abc", ""));

    /* clock_text / date_text: Unix seconds to UTC "HH:MM:SS" and "YYYY-MM-DD",
     * including the epoch, a leap day and the end of a year. */
    { char t[9], d[11];
      clock_text(0, t); date_text(0, d);
      CHECK(!strcmp(t, "00:00:00")); CHECK(!strcmp(d, "1970-01-01"));
      clock_text(1700000000UL, t); date_text(1700000000UL, d);
      CHECK(!strcmp(t, "22:13:20")); CHECK(!strcmp(d, "2023-11-14"));
      date_text(951782400UL, d); CHECK(!strcmp(d, "2000-02-29"));
      clock_text(1735689599UL, t); date_text(1735689599UL, d);
      CHECK(!strcmp(t, "23:59:59")); CHECK(!strcmp(d, "2024-12-31"));
      date_text(1735689600UL, d); CHECK(!strcmp(d, "2025-01-01")); }

    /* ip_text: a network-order IPv4 address (bytes as in a sockaddr) to text. */
    { static const unsigned char cases[4][4] = {{192, 168, 1, 97}, {10, 0, 0, 5}, {255, 255, 255, 255}, {0, 0, 0, 0}};
      static const char *want[4] = {"192.168.1.97", "10.0.0.5", "255.255.255.255", "0.0.0.0"};
      for (int i = 0; i < 4; i++) {
          u32 v; char ip[16];
          memcpy(&v, cases[i], 4);
          ip_text(v, ip);
          CHECK(!strcmp(ip, want[i]));
      } }
}
