/*
 * State streams, receiver, dedup window, reverse ACK queue, and the §6.4
 * interval statement (docs/DESIGN.md §6.2, §7.1-7.5). Hand-computed.
 * SPDX-License-Identifier: MIT
 */
#include "fltest.h"
#include "../core/fl_sender.h"
#include "../core/fl_receiver.h"
#include "../core/fl_frame.h"

static fl_sender_t S;
static fl_receiver_t R;

static void reset_sender(uint8_t n_streams)
{
    fl_sender_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.session_id = 7;
    cfg.n_streams = n_streams;
    cfg.policy.family = FL_POL_EDF_RR;
    cfg.ack_timeout_ms = 30;
    cfg.max_attempts = 3;
    CHECK_EQI(fl_sender_init(&S, &cfg), FL_OK);
}

static void reset_rx(uint8_t n_streams)
{
    CHECK_EQI(fl_receiver_init(&R, 7, n_streams), FL_OK);
}

static void pub(fl_time_t now, uint8_t stream, uint8_t v)
{
    uint8_t p[FL_STATE_PAYLOAD_LEN] = {0};
    p[0] = v;
    CHECK_EQI(fl_sender_publish_state(&S, now, stream, p, 0), FL_OK);
}

static fl_step_result_t step(fl_time_t now)
{
    fl_step_result_t r;
    CHECK_EQI(fl_sender_step(&S, now, &r), FL_OK);
    return r;
}

static uint32_t frame_seq(const uint8_t *f)
{
    return (uint32_t)f[6] | ((uint32_t)f[7] << 8) | ((uint32_t)f[8] << 16) | ((uint32_t)f[9] << 24);
}

static int sender_ack_state(fl_time_t now, uint8_t stream, uint32_t seq, fl_time_t gen)
{
    fl_frame_t f;
    uint8_t buf[FL_MAX_FRAME_LEN];
    uint8_t len = 0;
    fl_terminal_note_t note;
    memset(&f, 0, sizeof(f));
    f.type = FL_FT_ACK_STATE;
    f.session = 7;
    f.u.ack_state.stream = stream;
    f.u.ack_state.applied_seq = seq;
    f.u.ack_state.applied_gen = gen;
    CHECK_EQI(fl_frame_encode(&f, buf, (uint8_t)FL_MAX_FRAME_LEN, &len), FL_OK);
    return fl_sender_ingest_ack(&S, now, buf, len, &note);
}

static uint8_t mk_state(uint16_t session, uint8_t stream, uint32_t seq, fl_time_t gen, uint8_t *buf)
{
    fl_frame_t f;
    uint8_t len = 0;
    memset(&f, 0, sizeof(f));
    f.type = FL_FT_STATE;
    f.session = session;
    f.u.state.stream = stream;
    f.u.state.seq = seq;
    f.u.state.gen_time = gen;
    f.u.state.payload[0] = (uint8_t)seq;
    CHECK_EQI(fl_frame_encode(&f, buf, (uint8_t)FL_MAX_FRAME_LEN, &len), FL_OK);
    return len;
}

static uint8_t mk_event(uint32_t id, fl_time_t gen, fl_time_t deadline, uint8_t *buf)
{
    fl_frame_t f;
    uint8_t len = 0;
    memset(&f, 0, sizeof(f));
    f.type = FL_FT_EVENT;
    f.session = 7;
    f.u.event.id = id;
    f.u.event.kind = FL_EV_RAISE;
    f.u.event.code = 1;
    f.u.event.gen_time = gen;
    f.u.event.deadline_abs = deadline;
    CHECK_EQI(fl_frame_encode(&f, buf, (uint8_t)FL_MAX_FRAME_LEN, &len), FL_OK);
    return len;
}

static fl_rx_result_t rx_state(fl_time_t now, uint8_t stream, uint32_t seq, fl_time_t gen, int expect)
{
    uint8_t buf[FL_MAX_FRAME_LEN];
    uint8_t len = mk_state(7, stream, seq, gen, buf);
    fl_rx_result_t res;
    CHECK_EQI(fl_receiver_ingest(&R, now, buf, len, &res), expect);
    return res;
}

static fl_rx_result_t rx_event(fl_time_t now, uint32_t id, fl_time_t deadline)
{
    uint8_t buf[FL_MAX_FRAME_LEN];
    uint8_t len = mk_event(id, 0, deadline, buf);
    fl_rx_result_t res;
    CHECK_EQI(fl_receiver_ingest(&R, now, buf, len, &res), FL_OK);
    return res;
}

/* Runs one reverse slot; returns the decoded ACK or a frame with type 0 if none. */
static fl_frame_t rx_step(fl_time_t now)
{
    uint8_t buf[FL_MAX_FRAME_LEN];
    uint8_t len = 0;
    fl_frame_t f;
    memset(&f, 0, sizeof(f));
    CHECK_EQI(fl_receiver_step(&R, now, buf, (uint8_t)FL_MAX_FRAME_LEN, &len), FL_OK);
    if (len > 0) {
        CHECK_EQI(fl_frame_decode(buf, len, 7, &f), FL_OK);
    }
    return f;
}

/* (a) two publishes before a send: the first waiting snapshot is superseded; seq 2 goes out. */
static void test_latest_waiting_superseded(void)
{
    fl_step_result_t r;
    reset_sender(2);
    pub(0, 0, 1);
    pub(1, 0, 2);
    CHECK_EQ(S.stats.state_superseded, 1);
    CHECK_EQ(S.stats.state_published, 2);
    r = step(10);
    CHECK_EQ(r.choice.kind, FL_DK_STATE);
    CHECK_EQ(r.choice.stream, 0);
    CHECK_EQ(frame_seq(r.frame), 2);
    CHECK_EQ(r.seq, 2);
    CHECK_EQ(r.frame[14], 2); /* payload byte 0 */
}

/* (b) an in-flight copy is untouched by a later publish; the stream waits for ACK/timeout. */
static void test_inflight_untouched(void)
{
    fl_step_result_t r;
    fl_sched_view_t v;
    uint8_t saved[FL_MAX_FRAME_LEN];
    reset_sender(2);
    pub(0, 0, 1);
    r = step(10);
    CHECK_EQ(frame_seq(r.frame), 1);
    memcpy(saved, r.frame, r.frame_len);
    pub(11, 0, 2);
    CHECK_EQ(S.streams[0].inflight_seq, 1);
    CHECK_EQ(S.streams[0].inflight_valid, 1);
    CHECK_EQ(S.streams[0].latest_seq, 2);
    CHECK_EQ(S.stats.state_superseded, 0); /* seq 1 was sent, not superseded */
    fl_sender_build_view(&S, 12, &v);
    CHECK_EQ(v.n_streams, 0);
    r = step(12);
    CHECK_EQ(r.frame_len, 0);
    CHECK_EQI(sender_ack_state(15, 0, 1, 0), FL_OK);
    CHECK_EQ(S.streams[0].inflight_valid, 0);
    fl_sender_build_view(&S, 20, &v);
    CHECK_EQ(v.n_streams, 1);
    CHECK_EQ(v.streams[0].last_acked_gen, 0);
    CHECK_EQ(v.streams[0].last_tx_time, 10);
    r = step(20);
    CHECK_EQ(frame_seq(r.frame), 2);
    CHECK(memcmp(saved, r.frame, 6) == 0); /* header identical, body differs by seq */
}

/* (c) timeout then retry of the same latest seq. */
static void test_timeout_then_retry(void)
{
    fl_step_result_t r;
    reset_sender(1);
    pub(0, 0, 1);
    r = step(0);
    CHECK_EQ(frame_seq(r.frame), 1);
    r = step(10);
    CHECK_EQ(r.frame_len, 0);
    r = step(20);
    CHECK_EQ(r.frame_len, 0);
    r = step(30);
    CHECK_EQ(r.frame_len, 22);
    CHECK_EQ(frame_seq(r.frame), 1);
    CHECK_EQ(r.state_timeouts, 1);
    CHECK_EQ(S.stats.state_ack_timeouts, 1);
    CHECK_EQ(S.stats.state_tx, 2);
    CHECK_EQ(S.stats.bytes_data_tx, 44);
}

/* (d) ACK knowledge never regresses. */
static void test_ack_never_regresses(void)
{
    reset_sender(1);
    pub(0, 0, 1);
    step(0);
    CHECK_EQI(sender_ack_state(5, 0, 1, 0), FL_OK);
    pub(40, 0, 2);
    step(40);
    CHECK_EQI(sender_ack_state(45, 0, 2, 40), FL_OK);
    CHECK_EQ(S.streams[0].last_acked_seq, 2);
    CHECK_EQI(sender_ack_state(46, 0, 1, 0), FL_OK); /* reordered old ACK */
    CHECK_EQ(S.streams[0].last_acked_seq, 2);
    CHECK_EQ(S.streams[0].last_acked_gen, 40);
    CHECK_EQ(S.stats.acks_ok, 3);
    CHECK_EQ(S.stats.acks_impossible, 0);
}

/* (e)(n) receiver never regresses; the ACK reports the applied seq at ACK time. */
static void test_receiver_never_regresses(void)
{
    fl_rx_result_t res;
    fl_frame_t a;
    reset_rx(2);
    res = rx_state(0, 0, 5, 0, FL_OK); /* first frame applies even though seq != 1 */
    CHECK_EQ(res.outcome, FL_RX_STATE_APPLIED);
    reset_rx(2);
    res = rx_state(30, 0, 2, 20, FL_OK);
    CHECK_EQ(res.outcome, FL_RX_STATE_APPLIED);
    res = rx_state(31, 0, 1, 10, FL_OK);
    CHECK_EQ(res.outcome, FL_RX_STATE_STALE);
    CHECK_EQ(R.stats.state_stale_dropped, 1);
    CHECK_EQ(R.stats.state_applied, 1);
    CHECK_EQ(fl_receiver_state(&R, 0)->seq, 2);
    CHECK_EQ(fl_receiver_state(&R, 0)->gen_time, 20);
    CHECK_EQ(fl_receiver_state(&R, 0)->payload[0], 2);
    CHECK_EQ(R.stats.ack_state_coalesced, 1);
    a = rx_step(40);
    CHECK_EQ(a.type, FL_FT_ACK_STATE);
    CHECK_EQ(a.u.ack_state.stream, 0);
    CHECK_EQ(a.u.ack_state.applied_seq, 2);
    CHECK_EQ(a.u.ack_state.applied_gen, 20);
    a = rx_step(41);
    CHECK_EQ(a.type, 0);
    res = rx_state(42, 0, 2, 20, FL_OK); /* equal seq is stale too */
    CHECK_EQ(res.outcome, FL_RX_STATE_STALE);
    CHECK_EQ(fl_receiver_state(&R, 1)->valid, 0);
    CHECK(fl_receiver_state(&R, 2) == 0);
}

/* (f) dedup window semantics, exact boundary at distance 64. */
static void test_dedup_window(void)
{
    fl_rx_result_t res;
    fl_frame_t a;
    reset_rx(1);
    res = rx_event(0, 5, 100);
    CHECK_EQ(res.outcome, FL_RX_EVENT_DELIVERED);
    res = rx_event(1, 5, 100);
    CHECK_EQ(res.outcome, FL_RX_EVENT_DUPLICATE);
    CHECK_EQ(R.stats.ev_duplicate, 1);
    CHECK_EQ(R.stats.ev_delivered, 1);
    a = rx_step(1);
    CHECK_EQ(a.type, FL_FT_ACK_EVENT);
    CHECK_EQ(a.u.ack_event.id, 5);
    CHECK_EQ(a.u.ack_event.flags & FL_ACKF_DUPLICATE, 0);
    a = rx_step(2);
    CHECK_EQ(a.u.ack_event.id, 5);
    CHECK_EQ(a.u.ack_event.flags & FL_ACKF_DUPLICATE, FL_ACKF_DUPLICATE);
    res = rx_event(3, 3, 100); /* reordered but new, in window */
    CHECK_EQ(res.outcome, FL_RX_EVENT_DELIVERED);
    res = rx_event(4, 3, 100);
    CHECK_EQ(res.outcome, FL_RX_EVENT_DUPLICATE);
    CHECK_EQ(R.highest_id, 5);

    /* out-of-window boundary: highest 100; 36 is distance 64 -> rejected; 37 is 63 -> delivered */
    reset_rx(1);
    res = rx_event(0, 100, 1000);
    CHECK_EQ(res.outcome, FL_RX_EVENT_DELIVERED);
    a = rx_step(0);
    CHECK_EQ(a.u.ack_event.id, 100);
    CHECK_EQ(fl_receiver_ack_queue_len(&R), 0);
    res = rx_event(1, 36, 1000);
    CHECK_EQ(res.outcome, FL_RX_EVENT_OUT_OF_WINDOW);
    CHECK_EQ(R.stats.ev_out_of_window, 1);
    CHECK_EQ(R.stats.ev_delivered, 1);
    CHECK_EQ(fl_receiver_ack_queue_len(&R), 0); /* no ACK for it */
    a = rx_step(1);
    CHECK_EQ(a.type, 0);
    res = rx_event(2, 37, 1000);
    CHECK_EQ(res.outcome, FL_RX_EVENT_DELIVERED);
    CHECK_EQ(R.stats.ev_delivered, 2);
    res = rx_event(3, 36, 1000); /* still out of window */
    CHECK_EQ(res.outcome, FL_RX_EVENT_OUT_OF_WINDOW);

    /* big jump: bits shifted out; an id inside the new window is unknown -> delivered once */
    reset_rx(1);
    CHECK_EQ(rx_event(0, 1, 100).outcome, FL_RX_EVENT_DELIVERED);
    CHECK_EQ(rx_event(1, 1000, 100).outcome, FL_RX_EVENT_DELIVERED);
    CHECK_EQ(rx_event(2, 999, 100).outcome, FL_RX_EVENT_DELIVERED);
    CHECK_EQ(rx_event(3, 999, 100).outcome, FL_RX_EVENT_DUPLICATE);
    CHECK_EQ(rx_event(4, 1, 100).outcome, FL_RX_EVENT_OUT_OF_WINDOW);
    CHECK_EQ(rx_event(5, 937, 100).outcome, FL_RX_EVENT_DELIVERED);     /* distance 63 */
    CHECK_EQ(rx_event(6, 936, 100).outcome, FL_RX_EVENT_OUT_OF_WINDOW); /* distance 64 */
    /* jump of exactly 64 clears the whole window; the old highest becomes out-of-window */
    reset_rx(1);
    CHECK_EQ(rx_event(0, 10, 100).outcome, FL_RX_EVENT_DELIVERED);
    CHECK_EQ(rx_event(1, 74, 100).outcome, FL_RX_EVENT_DELIVERED);
    CHECK_EQ(rx_event(2, 10, 100).outcome, FL_RX_EVENT_OUT_OF_WINDOW);
    CHECK_EQ(rx_event(3, 11, 100).outcome, FL_RX_EVENT_DELIVERED); /* distance 63, never seen */
    /* id 0 is a field error */
    {
        uint8_t buf[FL_MAX_FRAME_LEN];
        uint8_t len = mk_event(0, 0, 100, buf);
        fl_rx_result_t r0;
        uint32_t ok_before = R.stats.frames_ok;
        CHECK_EQI(fl_receiver_ingest(&R, 4, buf, len, &r0), FL_ERR_FRAME_FIELD);
        CHECK_EQ(r0.outcome, FL_RX_REJECTED);
        CHECK_EQ(R.stats.frames_rejected, 1);
        CHECK_EQ(R.stats.frames_ok, ok_before);
    }
}

/* (g) deadline is inclusive at the receiver. */
static void test_deadline_inclusive(void)
{
    fl_rx_result_t res;
    reset_rx(1);
    res = rx_event(100, 1, 100);
    CHECK_EQ(res.outcome, FL_RX_EVENT_DELIVERED);
    CHECK_EQ(res.on_time, 1);
    res = rx_event(101, 2, 100);
    CHECK_EQ(res.outcome, FL_RX_EVENT_DELIVERED);
    CHECK_EQ(res.on_time, 0);
    CHECK_EQ(R.stats.ev_on_time, 1);
    CHECK_EQ(R.stats.ev_late, 1);
    CHECK_EQ(R.stats.ev_delivered, 2);
}

/* (h)(i) session mismatch and wrong-direction frames. */
static void test_rejections(void)
{
    uint8_t buf[FL_MAX_FRAME_LEN];
    uint8_t len;
    fl_rx_result_t res;
    fl_frame_t f;
    reset_rx(1);
    len = mk_state(8, 0, 1, 0, buf);
    CHECK_EQI(fl_receiver_ingest(&R, 0, buf, len, &res), FL_ERR_FRAME_SESSION);
    CHECK_EQ(res.outcome, FL_RX_REJECTED);
    CHECK_EQ(res.err, FL_ERR_FRAME_SESSION);
    CHECK_EQ(R.stats.session_mismatch, 1);
    CHECK_EQ(fl_receiver_state(&R, 0)->valid, 0);
    memset(&f, 0, sizeof(f));
    f.type = FL_FT_ACK_EVENT;
    f.session = 7;
    f.u.ack_event.id = 1;
    CHECK_EQI(fl_frame_encode(&f, buf, (uint8_t)FL_MAX_FRAME_LEN, &len), FL_OK);
    CHECK_EQI(fl_receiver_ingest(&R, 1, buf, len, &res), FL_ERR_FRAME_TYPE);
    CHECK_EQ(R.stats.frames_rejected, 1);
    len = mk_state(7, 1, 1, 0, buf); /* stream 1 >= n_streams 1 */
    CHECK_EQI(fl_receiver_ingest(&R, 2, buf, len, &res), FL_ERR_FRAME_FIELD);
    CHECK_EQ(R.stats.frames_rejected, 2);
    CHECK_EQ(R.stats.frames_ok, 0);
}

/* (j)(l) reverse service: event ACKs first, then state ACKs round robin; nothing leaves without a step. */
static void test_reverse_service_order(void)
{
    fl_frame_t a;
    reset_rx(2);
    rx_state(0, 0, 1, 0, FL_OK);
    rx_event(0, 1, 100);
    CHECK_EQ(fl_receiver_ack_queue_len(&R), 1);
    CHECK_EQ(R.stats.ack_tx, 0);
    a = rx_step(0);
    CHECK_EQ(a.type, FL_FT_ACK_EVENT);
    CHECK_EQ(a.u.ack_event.id, 1);
    a = rx_step(1);
    CHECK_EQ(a.type, FL_FT_ACK_STATE);
    CHECK_EQ(a.u.ack_state.stream, 0);
    a = rx_step(2);
    CHECK_EQ(a.type, 0);
    CHECK_EQ(R.stats.ack_tx, 2);
    CHECK_EQ(R.stats.bytes_ack_tx, 24);
    /* the cursor advanced past stream 0 above, so with both pending the order is 1 then 0,
       and again 1 then 0 (the cursor continues from the last served stream + 1, mod 2) */
    rx_state(3, 0, 2, 3, FL_OK);
    rx_state(3, 1, 1, 3, FL_OK);
    CHECK_EQ(rx_step(3).u.ack_state.stream, 1);
    CHECK_EQ(rx_step(4).u.ack_state.stream, 0);
    rx_state(5, 0, 3, 5, FL_OK);
    rx_state(5, 1, 2, 5, FL_OK);
    CHECK_EQ(rx_step(5).u.ack_state.stream, 1);
    CHECK_EQ(rx_step(6).u.ack_state.stream, 0);
    CHECK_EQ(rx_step(7).type, 0);
    /* only stream 1 pending while the cursor is at 0: served immediately */
    rx_state(8, 1, 3, 8, FL_OK);
    a = rx_step(8);
    CHECK_EQ(a.u.ack_state.stream, 1);
    CHECK_EQ(a.u.ack_state.applied_seq, 3);
}

/* (k) reverse-queue overflow: exactly one drop, recovered via retry/duplicate. */
static void test_reverse_queue_overflow(void)
{
    fl_rx_result_t res;
    fl_frame_t a;
    uint32_t i;
    reset_rx(1);
    for (i = 1; i <= FL_RX_ACK_QUEUE + 1u; i++) {
        res = rx_event(0, i, 100);
        CHECK_EQ(res.outcome, FL_RX_EVENT_DELIVERED);
        CHECK_EQ(res.ack_dropped, i == FL_RX_ACK_QUEUE + 1u ? 1u : 0u);
    }
    CHECK_EQ(R.stats.ack_event_dropped, 1);
    CHECK_EQ(R.stats.ev_delivered, FL_RX_ACK_QUEUE + 1u);
    CHECK_EQ(fl_receiver_ack_queue_len(&R), FL_RX_ACK_QUEUE);
    for (i = 1; i <= FL_RX_ACK_QUEUE; i++) {
        a = rx_step(i);
        CHECK_EQ(a.type, FL_FT_ACK_EVENT);
        CHECK_EQ(a.u.ack_event.id, i);
    }
    a = rx_step(FL_RX_ACK_QUEUE + 1u);
    CHECK_EQ(a.type, 0);
    res = rx_event(FL_RX_ACK_QUEUE + 2u, FL_RX_ACK_QUEUE + 1u, 100); /* sender retries the unacked one */
    CHECK_EQ(res.outcome, FL_RX_EVENT_DUPLICATE);
    CHECK_EQ(res.ack_dropped, 0);
    a = rx_step(FL_RX_ACK_QUEUE + 3u);
    CHECK_EQ(a.type, FL_FT_ACK_EVENT);
    CHECK_EQ(a.u.ack_event.id, FL_RX_ACK_QUEUE + 1u);
    CHECK_EQ(a.u.ack_event.flags & FL_ACKF_DUPLICATE, FL_ACKF_DUPLICATE);
    /* ring wrap-around: fill, drain two, fill two more, drain all in FIFO order */
    reset_rx(1);
    for (i = 1; i <= FL_RX_ACK_QUEUE; i++) rx_event(0, i, 100);
    CHECK_EQ(rx_step(1).u.ack_event.id, 1);
    CHECK_EQ(rx_step(2).u.ack_event.id, 2);
    rx_event(3, FL_RX_ACK_QUEUE + 1u, 100);
    rx_event(3, FL_RX_ACK_QUEUE + 2u, 100);
    CHECK_EQ(R.stats.ack_event_dropped, 0);
    for (i = 3; i <= FL_RX_ACK_QUEUE + 2u; i++) {
        CHECK_EQ(rx_step(i).u.ack_event.id, i);
    }
    CHECK_EQ(rx_step(FL_RX_ACK_QUEUE + 3u).type, 0);
}

/*
 * (m) §6.4 interval statement on a scripted exchange: publish every 20 ms,
 * deliver every data frame, drop every other ACK; at every ms the receiver's
 * applied seq must lie in [last_acked_seq, max_sent_seq].
 */
static void test_interval_statement(void)
{
    fl_time_t t;
    uint32_t acks = 0;
    reset_sender(1);
    reset_rx(1);
    for (t = 0; t < 400; t++) {
        uint32_t rx_seq;
        if (t % 20 == 0) {
            pub(t, 0, (uint8_t)(t / 20));
        }
        if (t % 10 == 0) {
            fl_step_result_t r = step(t);
            uint8_t abuf[FL_MAX_FRAME_LEN];
            uint8_t alen = 0;
            if (r.frame_len > 0) {
                fl_rx_result_t res;
                CHECK_EQI(fl_receiver_ingest(&R, t, r.frame, r.frame_len, &res), FL_OK);
            }
            CHECK_EQI(fl_receiver_step(&R, t, abuf, (uint8_t)FL_MAX_FRAME_LEN, &alen), FL_OK);
            if (alen > 0) {
                acks++;
                if (acks % 2 == 0) { /* every other ACK is lost */
                    fl_terminal_note_t note;
                    CHECK_EQI(fl_sender_ingest_ack(&S, t, abuf, alen, &note), FL_OK);
                }
            }
        }
        rx_seq = fl_receiver_state(&R, 0)->valid ? fl_receiver_state(&R, 0)->seq : 0u;
        CHECK(rx_seq >= S.streams[0].last_acked_seq);
        CHECK(rx_seq <= S.streams[0].max_sent_seq);
    }
    CHECK(acks > 5);
    CHECK(S.streams[0].last_acked_seq < S.streams[0].max_sent_seq || S.streams[0].last_acked_seq > 0);
}

int main(void)
{
    RUN(test_latest_waiting_superseded);
    RUN(test_inflight_untouched);
    RUN(test_timeout_then_retry);
    RUN(test_ack_never_regresses);
    RUN(test_receiver_never_regresses);
    RUN(test_dedup_window);
    RUN(test_deadline_inclusive);
    RUN(test_rejections);
    RUN(test_reverse_service_order);
    RUN(test_reverse_queue_overflow);
    RUN(test_interval_statement);
    return FLTEST_REPORT("test_state_rx");
}
