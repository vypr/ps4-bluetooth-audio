#include "test.h"
#include "conn.h"

void test_conn(void) {
    /* Backoff schedule: 2, 4, 8, 16 s for the first four failures, then 0
     * ("give up and ask the user"). */
    CHECK_EQ(conn_backoff_us(1), 2000000);
    CHECK_EQ(conn_backoff_us(2), 4000000);
    CHECK_EQ(conn_backoff_us(3), 8000000);
    CHECK_EQ(conn_backoff_us(4), 16000000);
    CHECK_EQ(conn_backoff_us(5), 0);
    CHECK_EQ(conn_backoff_us(0), 0);
    CHECK_EQ(conn_give_up(4), 0);
    CHECK_EQ(conn_give_up(5), 1);

    /* SBC mode mapping and the one-shot flip used for automatic fallback. */
    CHECK_EQ(conn_send_type(SBC_JOINT), 4);
    CHECK_EQ(conn_send_type(SBC_DUAL), 3);
    CHECK_EQ(conn_other_mode(SBC_JOINT), SBC_DUAL);
    CHECK_EQ(conn_other_mode(SBC_DUAL), SBC_JOINT);

    /* Rest-mode resume: a gap beyond the threshold means the console woke. */
    CHECK_EQ(conn_resumed(0, 9000000, CONN_RESUME_GAP_US), 0);          /* no prior tick */
    CHECK_EQ(conn_resumed(1000000, 3000000, CONN_RESUME_GAP_US), 0);    /* 2 s gap */
    CHECK_EQ(conn_resumed(1000000, 9000000, CONN_RESUME_GAP_US), 1);    /* 8 s gap */
    CHECK_EQ(conn_resumed(9000000, 1000000, CONN_RESUME_GAP_US), 0);    /* clock went back */

    /* slept: the process clock stops in rest mode while the wall clock runs on. */
    CHECK_EQ(conn_slept(4000, 4000, CONN_RESUME_GAP_US), 0);              /* normal tick */
    CHECK_EQ(conn_slept(8000000, 8100000, CONN_RESUME_GAP_US), 0);        /* starved thread: both advance */
    CHECK_EQ(conn_slept(4000, 720000000, CONN_RESUME_GAP_US), 1);         /* 12 min of rest mode */
    CHECK_EQ(conn_slept(4000, 4000 + CONN_RESUME_GAP_US, CONN_RESUME_GAP_US), 0); /* exactly the gap */
    CHECK_EQ(conn_slept(4000, 0, CONN_RESUME_GAP_US), 0);                 /* wall clock set back */

    /* Public link state from the explicit state, with the dropped/gave-up flags. */
    CHECK_EQ(conn_link_state(CONN_STREAMING, 0, 0), BT_LINK_STREAMING);
    CHECK_EQ(conn_link_state(CONN_STREAMING, 1, 1), BT_LINK_STREAMING); /* streaming wins */
    CHECK_EQ(conn_link_state(CONN_IDLE, 0, 0), BT_LINK_IDLE);
    CHECK_EQ(conn_link_state(CONN_IDLE, 0, 1), BT_LINK_FAILED);         /* gave up, waiting */
    CHECK_EQ(conn_link_state(CONN_DISCOVERING, 0, 0), BT_LINK_SEARCHING);
    CHECK_EQ(conn_link_state(CONN_RECOVERING, 1, 0), BT_LINK_LOST);     /* dropped mid-stream */
    CHECK_EQ(conn_link_state(CONN_RECOVERING, 0, 0), BT_LINK_FAILED);   /* a connect failed */
    CHECK_EQ(conn_link_state(CONN_CONNECTING, 0, 0), BT_LINK_CONNECTING);
    CHECK_EQ(conn_link_state(CONN_PAIRING, 0, 0), BT_LINK_CONNECTING);
    CHECK_EQ(conn_link_state(CONN_CONFIGURING, 0, 0), BT_LINK_CONNECTING);

    /* due: an attempt waiting only for the radio shows as connecting; a pending
     * backoff, a lost link or a give-up don't. */
    CHECK_EQ(conn_due(CONN_RECOVERING, 0, 0, 1000000, 2000000), 1);
    CHECK_EQ(conn_due(CONN_RECOVERING, 0, 0, 3000000, 2000000), 0); /* backing off */
    CHECK_EQ(conn_due(CONN_RECOVERING, 1, 0, 1000000, 2000000), 0); /* lost mid-stream */
    CHECK_EQ(conn_due(CONN_RECOVERING, 0, 1, 1000000, 2000000), 0); /* gave up */
    CHECK_EQ(conn_due(CONN_IDLE, 0, 0, 0, 2000000), 0);

    /* retry_in_ms: only pending while recovering, not given up, with a future
     * next_try; reported in milliseconds. */
    CHECK_EQ(conn_retry_in_ms(CONN_RECOVERING, 0, 5000000, 1000000), (5000000 - 1000000) / 1000);
    CHECK_EQ(conn_retry_in_ms(CONN_RECOVERING, 1, 5000000, 1000000), 0); /* gave up */
    CHECK_EQ(conn_retry_in_ms(CONN_CONNECTING, 0, 5000000, 1000000), 0); /* mid-attempt */
    CHECK_EQ(conn_retry_in_ms(CONN_STREAMING, 0, 5000000, 1000000), 0);  /* streaming */
    CHECK_EQ(conn_retry_in_ms(CONN_RECOVERING, 0, 1000000, 5000000), 0); /* already due */

    /* quiet_for_ms: 0 unless streaming and now >= ok_at; clamped. */
    CHECK_EQ(conn_quiet_for_ms(CONN_RECOVERING, 1000000, 5000000), 0);   /* not streaming */
    CHECK_EQ(conn_quiet_for_ms(CONN_STREAMING, 5000000, 1000000), 0);    /* now before ok_at */
    CHECK_EQ(conn_quiet_for_ms(CONN_STREAMING, 1000000, 3000000), 2000); /* (3s-1s)/1000 */
    CHECK_EQ(conn_quiet_for_ms(CONN_STREAMING, 0, 3000000000000ULL), 2000000000); /* clamp */

    /* plan_link: dial when there is no link; reuse only a link this run dialled;
     * any other live link (an earlier run's, or one still closing) is dropped and
     * dialled afresh, since it can play nothing while accepting audio. */
    CHECK_EQ(conn_plan_link(0, 0), CONN_DIAL);
    CHECK_EQ(conn_plan_link(4, 1), CONN_DIAL);
    CHECK_EQ(conn_plan_link(7, 1), CONN_DIAL);
    CHECK_EQ(conn_plan_link(-1, 0), CONN_DIAL);       /* driver error: just dial */
    CHECK_EQ(conn_plan_link(5, 1), CONN_REUSE);
    CHECK_EQ(conn_plan_link(6, 1), CONN_REUSE);
    CHECK_EQ(conn_plan_link(5, 0), CONN_DROP_FIRST);
    CHECK_EQ(conn_plan_link(6, 0), CONN_DROP_FIRST);
    CHECK(conn_link_up(5) && conn_link_up(6) && !conn_link_up(4) && !conn_link_up(7));

    /* paging: a reading during a dial shows the attempt only when it differs from
     * the one before the dial and is not a link. */
    CHECK_EQ(conn_paging(1, 1), 0);
    CHECK_EQ(conn_paging(3, 1), 1);
    CHECK_EQ(conn_paging(0, 1), 1);
    CHECK_EQ(conn_paging(5, 1), 0);
    CHECK_EQ(conn_paging(-1, 1), 1);

    /* A dial is over once its state is back to the pre-dial one after showing it
     * (1 -> 2 -> 1); not before it was seen, and not while linked. */
    CHECK_EQ(conn_dial_ended(1, 1, 1), 1);
    CHECK_EQ(conn_dial_ended(1, 1, 0), 0);
    CHECK_EQ(conn_dial_ended(2, 1, 1), 0);
    CHECK_EQ(conn_dial_ended(6, 1, 1), 0);

    /* A cancelled dial: confirmed only when the attempt was seen and the state is
     * back to the one before; else it waits out the dial's window. */
    u64 since = 10000000, end = since + CONN_PAGE_END_US;
    CHECK_EQ(conn_hold_next(CONN_HOLD_PAGE, 3, 1, 1, since + 100000, since, end), CONN_HOLD_WAIT);
    CHECK_EQ(conn_hold_next(CONN_HOLD_PAGE, 1, 1, 1, since + 300000, since, end), CONN_HOLD_CANCELLED);
    CHECK_EQ(conn_hold_next(CONN_HOLD_PAGE, 1, 1, 0, since + 300000, since, end), CONN_HOLD_WAIT); /* never seen: no proof */
    CHECK_EQ(conn_hold_next(CONN_HOLD_PAGE, 1, 1, 0, end, since, end), CONN_HOLD_WINDOW_OVER);
    CHECK_EQ(conn_hold_next(CONN_HOLD_PAGE, 3, 1, 1, end + 1, since, end), CONN_HOLD_WINDOW_OVER); /* still showing it */
    CHECK_EQ(conn_hold_next(CONN_HOLD_PAGE, -1, 1, 1, since + 300000, since, end), CONN_HOLD_WAIT); /* no reading */
    CHECK_EQ(conn_hold_next(CONN_HOLD_PAGE, 6, 1, 1, since + 300000, since, end), CONN_HOLD_LINKED);
    /* A dropped link: done once down; stuck after CONN_HOLD_LINK_US still up. */
    CHECK_EQ(conn_hold_next(CONN_HOLD_LINK, 6, 6, 0, since + 500000, since, 0), CONN_HOLD_WAIT);
    CHECK_EQ(conn_hold_next(CONN_HOLD_LINK, 1, 6, 0, since + 500000, since, 0), CONN_HOLD_DOWN);
    CHECK_EQ(conn_hold_next(CONN_HOLD_LINK, -1, 6, 0, since + 500000, since, 0), CONN_HOLD_DOWN); /* driver closed */
    CHECK_EQ(conn_hold_next(CONN_HOLD_LINK, 5, 6, 0, since + CONN_HOLD_LINK_US, since, 0), CONN_HOLD_STUCK);
    CHECK_EQ(conn_hold_next(CONN_HOLD_NONE, 6, 1, 0, since, since, 0), CONN_HOLD_DOWN);

    /* Add Device ends when the console app stops asking (closed, or a game in
     * front), or a phone's search has been over for a minute. */
    CHECK_EQ(conn_add_over(CONN_ADD_CONSOLE, 20000000, 15000000, 0, 1), 0);
    CHECK_EQ(conn_add_over(CONN_ADD_CONSOLE, 26000000, 15000000, 0, 1), 1);
    CHECK_EQ(conn_add_over(CONN_ADD_CONSOLE, 15000000, 20000000, 0, 0), 0); /* asked after `now` was read */
    CHECK_EQ(conn_add_over(CONN_ADD_PHONE, 80000000, 0, 10000000, 0), 1);
    CHECK_EQ(conn_add_over(CONN_ADD_PHONE, 80000000, 0, 10000000, 1), 0); /* still searching */
    CHECK_EQ(conn_add_over(CONN_ADD_PHONE, 50000000, 0, 10000000, 0), 0);
    CHECK_EQ(conn_add_over(CONN_ADD_NONE, 99000000, 0, 0, 0), 0);
}
