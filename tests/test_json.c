#include "test.h"
#include "json.h"
#include <string.h>

void test_json(void) {
    char buf[256];

    /* number: zero, positive, negative, and a large value. */
    { struct writer w = {buf, 0, sizeof buf}; number(&w, 0);        CHECK(!strcmp(buf, "0")); }
    { struct writer w = {buf, 0, sizeof buf}; number(&w, 42);       CHECK(!strcmp(buf, "42")); }
    { struct writer w = {buf, 0, sizeof buf}; number(&w, -7);       CHECK(!strcmp(buf, "-7")); }
    { struct writer w = {buf, 0, sizeof buf}; number(&w, 2147483648L); CHECK(!strcmp(buf, "2147483648")); }

    /* field: leading comma, quoted key, numeric value. */
    { struct writer w = {buf, 0, sizeof buf}; field(&w, "volume", 40); CHECK(!strcmp(buf, ",\"volume\":40")); }

    /* quoted: escapes " and backslash, maps non-printables to '?'. */
    { struct writer w = {buf, 0, sizeof buf}; quoted(&w, "a\"b\\c");   CHECK(!strcmp(buf, "\"a\\\"b\\\\c\"")); }
    { struct writer w = {buf, 0, sizeof buf}; quoted(&w, "x\ty");      CHECK(!strcmp(buf, "\"x?y\"")); }

    /* quoted clips the payload to 127 characters. */
    { char in[200]; memset(in, 'z', sizeof in); in[199] = 0;
      struct writer w = {buf, 0, sizeof buf}; quoted(&w, in);
      CHECK_EQ(strlen(buf), 127 + 2); /* 127 z's plus the two quotes */ }

    /* append never overruns the capacity and always NUL-terminates. */
    { char small[5]; struct writer w = {small, 0, sizeof small};
      append(&w, "abcdefgh");
      CHECK(w.used == 4);                 /* cap-1 bytes written */
      CHECK(small[4] == 0); }

    /* A realistic status-style fragment composes correctly. */
    { struct writer w = {buf, 0, sizeof buf};
      append(&w, "{\"message\":");
      quoted(&w, "Ready");
      field(&w, "volume", 40);
      field(&w, "streaming", 0);
      append(&w, "}");
      CHECK(!strcmp(buf, "{\"message\":\"Ready\",\"volume\":40,\"streaming\":0}")); }
}
