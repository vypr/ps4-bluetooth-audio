#include "test.h"
#include "timing.h"
#include <string.h>

void test_timing(void) {
    struct bta_second secs[128];
    struct bta_second cur;
    u32 seq;
    u64 start;

    /* First roll with no prior second: opens a second, records nothing, keeps seq. */
    memset(secs, 0, sizeof secs);
    cur = (struct bta_second){0};
    cur.pkts = 99; /* garbage that must be cleared */
    seq = 0; start = 0;
    bta_roll_second(secs, &cur, &seq, &start, 1000);
    CHECK_EQ(seq, 0);
    CHECK_EQ(start, 1000);
    CHECK_EQ(cur.pkts, 0);

    /* Within the same second: nothing changes. */
    cur.pkts = 5; seq = 0; start = 1000000;
    bta_roll_second(secs, &cur, &seq, &start, 1000000 + 500000);
    CHECK_EQ(seq, 0);
    CHECK_EQ(start, 1000000);
    CHECK_EQ(cur.pkts, 5);

    /* After a full second with traffic: the second is recorded and seq advances. */
    cur = (struct bta_second){0};
    cur.pkts = 5; cur.refused = 1;
    seq = 0; start = 1000000;
    bta_roll_second(secs, &cur, &seq, &start, 2500000);
    CHECK_EQ(seq, 1);
    CHECK_EQ(secs[1].pkts, 5);
    CHECK_EQ(secs[1].refused, 1);
    CHECK_EQ(secs[1].seq, 1);
    CHECK_EQ(cur.pkts, 0);       /* current reset */
    CHECK_EQ(cur.t, 2);          /* 2500000 / 1e6 */
    CHECK_EQ(start, 2500000);

    /* After a full second with no traffic: reset but record nothing. */
    cur = (struct bta_second){0};
    seq = 1; start = 1000000;
    bta_roll_second(secs, &cur, &seq, &start, 2500000);
    CHECK_EQ(seq, 1);            /* unchanged: empty second dropped */
    CHECK_EQ(start, 2500000);

    /* timing_json: empty history. */
    char buf[4096];
    memset(secs, 0, sizeof secs);
    unsigned n = bta_timing_json(secs, 0, 100, buf, sizeof buf);
    CHECK(!strcmp(buf, "{\"seq\":0,\"now_s\":100,\"secs\":[]}"));
    CHECK_EQ(n, (long)strlen(buf));

    /* timing_json: two recorded seconds, oldest first, 12 values each. */
    memset(secs, 0, sizeof secs);
    secs[1] = (struct bta_second){.seq = 1, .t = 10, .pkts = 2};
    secs[2] = (struct bta_second){.seq = 2, .t = 11, .pkts = 3};
    bta_timing_json(secs, 2, 100, buf, sizeof buf);
    CHECK(!strcmp(buf,
        "{\"seq\":2,\"now_s\":100,\"secs\":["
        "[1,10,2,0,0,0,0,0,0,0,0,0],"
        "[2,11,3,0,0,0,0,0,0,0,0,0]]}"));
}
