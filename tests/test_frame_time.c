/*
 * Wire codec and time/identity boundaries (docs/DESIGN.md §3, §4, §5).
 * Expected values are derived from the spec tables, not from running the code:
 * sizes 22/27/14/10, horizon 2^31-1 = 2147483647, FL_MAX_REL_MS = 2^30.
 * SPDX-License-Identifier: MIT
 */
#include "fltest.h"
#include "../core/fl_frame.h"
#include "../core/fl_sender.h"
#include "../core/fl_receiver.h"

static fl_sender_t S;
static fl_receiver_t R;

static void sender_reset(uint8_t n_streams)
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

static void test_sizes(void)
{
    CHECK_EQ(fl_frame_size(FL_FT_STATE), 22);
    CHECK_EQ(fl_frame_size(FL_FT_EVENT), 27);
    CHECK_EQ(fl_frame_size(FL_FT_ACK_STATE), 14);
    CHECK_EQ(fl_frame_size(FL_FT_ACK_EVENT), 10);
    CHECK_EQ(fl_frame_size(0), 0);
    CHECK_EQ(fl_frame_size(5), 0);
    CHECK_EQ(fl_frame_size(0xFF), 0);
}

/* STATE: magic F1, type 01, session 07 00, len 16, stream, seq LE, gen LE, payload. */
static void test_round_trip_state_le(void)
{
    fl_frame_t f, g;
    uint8_t buf[FL_MAX_FRAME_LEN];
    uint8_t len = 0;
    uint8_t i;
    memset(&f, 0, sizeof(f));
    f.type = FL_FT_STATE;
    f.session = 0x0107;
    f.u.state.stream = 3;
    f.u.state.seq = 0x01020304u;
    f.u.state.gen_time = 0xA0B0C0D0u;
    for (i = 0; i < 8; i++) f.u.state.payload[i] = (uint8_t)(0x10u + i);
    CHECK_EQI(fl_frame_encode(&f, buf, (uint8_t)FL_MAX_FRAME_LEN, &len), FL_OK);
    CHECK_EQ(len, 22);
    CHECK_EQ(buf[0], 0xF1);
    CHECK_EQ(buf[1], 1);
    CHECK_EQ(buf[2], 0x07);
    CHECK_EQ(buf[3], 0x01);
    CHECK_EQ(buf[4], 22);
    CHECK_EQ(buf[5], 3);
    CHECK_EQ(buf[6], 0x04);
    CHECK_EQ(buf[7], 0x03);
    CHECK_EQ(buf[8], 0x02);
    CHECK_EQ(buf[9], 0x01);
    CHECK_EQ(buf[10], 0xD0);
    CHECK_EQ(buf[13], 0xA0);
    CHECK_EQ(buf[14], 0x10);
    CHECK_EQ(buf[21], 0x17);
    CHECK_EQI(fl_frame_decode(buf, len, 0x0107, &g), FL_OK);
    CHECK_EQ(g.type, FL_FT_STATE);
    CHECK_EQ(g.session, 0x0107);
    CHECK_EQ(g.u.state.stream, 3);
    CHECK_EQ(g.u.state.seq, 0x01020304u);
    CHECK_EQ(g.u.state.gen_time, 0xA0B0C0D0u);
    CHECK(memcmp(g.u.state.payload, f.u.state.payload, 8) == 0);
}

static void test_round_trip_event(void)
{
    fl_frame_t f, g;
    uint8_t buf[FL_MAX_FRAME_LEN];
    uint8_t len = 0;
    uint8_t i;
    memset(&f, 0, sizeof(f));
    f.type = FL_FT_EVENT;
    f.session = 7;
    f.u.event.id = 0xFFFFFFFEu;
    f.u.event.kind = FL_EV_CLEAR;
    f.u.event.code = 200;
    f.u.event.gen_time = 12345;
    f.u.event.deadline_abs = 0xBFFFFFFFu;
    for (i = 0; i < 8; i++) f.u.event.payload[i] = (uint8_t)(0xF0u - i);
    CHECK_EQI(fl_frame_encode(&f, buf, (uint8_t)FL_MAX_FRAME_LEN, &len), FL_OK);
    CHECK_EQ(len, 27);
    CHECK_EQ(buf[4], 27);
    CHECK_EQ(buf[5], 0xFE);
    CHECK_EQ(buf[8], 0xFF);
    CHECK_EQ(buf[9], FL_EV_CLEAR);
    CHECK_EQ(buf[10], 200);
    CHECK_EQ(buf[11], 0x39); /* 12345 = 0x3039 */
    CHECK_EQ(buf[12], 0x30);
    CHECK_EQ(buf[15], 0xFF);
    CHECK_EQ(buf[18], 0xBF);
    CHECK_EQI(fl_frame_decode(buf, len, 7, &g), FL_OK);
    CHECK_EQ(g.u.event.id, 0xFFFFFFFEu);
    CHECK_EQ(g.u.event.kind, FL_EV_CLEAR);
    CHECK_EQ(g.u.event.code, 200);
    CHECK_EQ(g.u.event.gen_time, 12345);
    CHECK_EQ(g.u.event.deadline_abs, 0xBFFFFFFFu);
    CHECK(memcmp(g.u.event.payload, f.u.event.payload, 8) == 0);
}

static void test_round_trip_acks(void)
{
    fl_frame_t f, g;
    uint8_t buf[FL_MAX_FRAME_LEN];
    uint8_t len = 0;
    memset(&f, 0, sizeof(f));
    f.type = FL_FT_ACK_STATE;
    f.session = 65535;
    f.u.ack_state.stream = 7;
    f.u.ack_state.applied_seq = 99;
    f.u.ack_state.applied_gen = 100000;
    CHECK_EQI(fl_frame_encode(&f, buf, (uint8_t)FL_MAX_FRAME_LEN, &len), FL_OK);
    CHECK_EQ(len, 14);
    CHECK_EQ(buf[2], 0xFF);
    CHECK_EQ(buf[3], 0xFF);
    CHECK_EQI(fl_frame_decode(buf, len, 65535, &g), FL_OK);
    CHECK_EQ(g.u.ack_state.stream, 7);
    CHECK_EQ(g.u.ack_state.applied_seq, 99);
    CHECK_EQ(g.u.ack_state.applied_gen, 100000);
    memset(&f, 0, sizeof(f));
    f.type = FL_FT_ACK_EVENT;
    f.session = 0;
    f.u.ack_event.id = 1;
    f.u.ack_event.flags = FL_ACKF_DUPLICATE;
    CHECK_EQI(fl_frame_encode(&f, buf, (uint8_t)FL_MAX_FRAME_LEN, &len), FL_OK);
    CHECK_EQ(len, 10);
    CHECK_EQ(buf[9], 1);
    CHECK_EQI(fl_frame_decode(buf, len, 0, &g), FL_OK);
    CHECK_EQ(g.u.ack_event.id, 1);
    CHECK_EQ(g.u.ack_event.flags, FL_ACKF_DUPLICATE);
}

static void good_state_frame(uint8_t *buf, uint8_t *len)
{
    fl_frame_t f;
    memset(&f, 0, sizeof(f));
    f.type = FL_FT_STATE;
    f.session = 7;
    f.u.state.stream = 0;
    f.u.state.seq = 1;
    CHECK_EQI(fl_frame_encode(&f, buf, (uint8_t)FL_MAX_FRAME_LEN, len), FL_OK);
}

static void test_decode_rejections(void)
{
    uint8_t buf[FL_MAX_FRAME_LEN];
    uint8_t len = 0;
    fl_frame_t g;
    good_state_frame(buf, &len);
    CHECK_EQI(fl_frame_decode(buf, 4, 7, &g), FL_ERR_FRAME_LEN);
    CHECK_EQI(fl_frame_decode(buf, 0, 7, &g), FL_ERR_FRAME_LEN);
    CHECK_EQI(fl_frame_decode(buf, (uint8_t)(len - 1u), 7, &g), FL_ERR_FRAME_LEN);
    CHECK_EQI(fl_frame_decode(buf, (uint8_t)(len + 1u), 7, &g), FL_ERR_FRAME_LEN);
    buf[0] = 0xF0;
    CHECK_EQI(fl_frame_decode(buf, len, 7, &g), FL_ERR_FRAME_MAGIC);
    good_state_frame(buf, &len);
    buf[1] = 9;
    CHECK_EQI(fl_frame_decode(buf, len, 7, &g), FL_ERR_FRAME_TYPE);
    good_state_frame(buf, &len);
    buf[4] = 21; /* declared length disagrees with the type size */
    CHECK_EQI(fl_frame_decode(buf, len, 7, &g), FL_ERR_FRAME_LEN);
    good_state_frame(buf, &len);
    CHECK_EQI(fl_frame_decode(buf, len, 8, &g), FL_ERR_FRAME_SESSION);
    good_state_frame(buf, &len);
    buf[5] = (uint8_t)FL_MAX_STREAMS;
    CHECK_EQI(fl_frame_decode(buf, len, 7, &g), FL_ERR_FRAME_FIELD);
    /* ACK_STATE stream out of range */
    {
        fl_frame_t f;
        memset(&f, 0, sizeof(f));
        f.type = FL_FT_ACK_STATE;
        f.session = 7;
        f.u.ack_state.stream = (uint8_t)FL_MAX_STREAMS;
        CHECK_EQI(fl_frame_encode(&f, buf, (uint8_t)FL_MAX_FRAME_LEN, &len), FL_ERR_FRAME_FIELD);
        f.u.ack_state.stream = 0;
        CHECK_EQI(fl_frame_encode(&f, buf, (uint8_t)FL_MAX_FRAME_LEN, &len), FL_OK);
        buf[5] = (uint8_t)FL_MAX_STREAMS;
        CHECK_EQI(fl_frame_decode(buf, len, 7, &g), FL_ERR_FRAME_FIELD);
    }
    /* EVENT kind 0 and 3 */
    {
        fl_frame_t f;
        memset(&f, 0, sizeof(f));
        f.type = FL_FT_EVENT;
        f.session = 7;
        f.u.event.id = 1;
        f.u.event.kind = FL_EV_RAISE;
        CHECK_EQI(fl_frame_encode(&f, buf, (uint8_t)FL_MAX_FRAME_LEN, &len), FL_OK);
        buf[9] = 0;
        CHECK_EQI(fl_frame_decode(buf, len, 7, &g), FL_ERR_FRAME_FIELD);
        buf[9] = 3;
        CHECK_EQI(fl_frame_decode(buf, len, 7, &g), FL_ERR_FRAME_FIELD);
    }
    /* encode with a too-small buffer, and an unknown type */
    {
        fl_frame_t f;
        memset(&f, 0, sizeof(f));
        f.type = FL_FT_EVENT;
        f.session = 7;
        f.u.event.kind = FL_EV_RAISE;
        CHECK_EQI(fl_frame_encode(&f, buf, 26, &len), FL_ERR_FRAME_LEN);
        f.type = 9;
        CHECK_EQI(fl_frame_encode(&f, buf, (uint8_t)FL_MAX_FRAME_LEN, &len), FL_ERR_FRAME_TYPE);
        CHECK_EQI(fl_frame_encode(0, buf, (uint8_t)FL_MAX_FRAME_LEN, &len), FL_ERR_ARG);
        CHECK_EQI(fl_frame_decode(0, 10, 7, &g), FL_ERR_ARG);
    }
}

/* An exactly sized buffer followed by a canary: a short supply must not read or write past it. */
static void test_decode_short_buffer_canary(void)
{
    struct {
        uint8_t buf[22];
        uint8_t canary;
    } b;
    uint8_t tmp[FL_MAX_FRAME_LEN];
    uint8_t len = 0;
    fl_frame_t g;
    good_state_frame(tmp, &len);
    memcpy(b.buf, tmp, 22);
    b.canary = 0x5A;
    CHECK_EQI(fl_frame_decode(b.buf, 21, 7, &g), FL_ERR_FRAME_LEN);
    CHECK_EQI(fl_frame_decode(b.buf, 22, 7, &g), FL_OK);
    CHECK_EQ(b.canary, 0x5A);
    CHECK_EQ(b.buf[0], 0xF1);
}

static void test_time_horizon_and_regress(void)
{
    uint8_t payload[FL_STATE_PAYLOAD_LEN] = {0};
    uint8_t evp[FL_EVENT_PAYLOAD_LEN] = {0};
    fl_step_result_t sr;
    fl_rx_result_t rr;
    fl_terminal_note_t note;
    uint8_t ack[FL_MAX_FRAME_LEN];
    uint8_t alen = 0;
    uint8_t frame[FL_MAX_FRAME_LEN];
    uint8_t flen = 0;
    uint32_t id = 0;
    sender_reset(1);
    CHECK_EQI(fl_sender_publish_state(&S, FL_TIME_HORIZON_MS + 1u, 0, payload, 0), FL_ERR_TIME_HORIZON);
    CHECK_EQ(S.stats.state_published, 0);
    CHECK_EQI(fl_sender_publish_state(&S, 100, 0, payload, 0), FL_OK);
    CHECK_EQI(fl_sender_publish_state(&S, 99, 0, payload, 0), FL_ERR_TIME_REGRESS);
    CHECK_EQ(S.stats.state_published, 1);
    CHECK_EQ(S.streams[0].next_seq, 2);
    CHECK_EQI(fl_sender_publish_state(&S, 100, 0, payload, 0), FL_OK); /* equal time allowed */
    CHECK_EQI(fl_sender_step(&S, 99, &sr), FL_ERR_TIME_REGRESS);
    CHECK_EQ(S.stats.steps, 0);
    CHECK_EQI(fl_sender_step(&S, FL_TIME_HORIZON_MS + 1u, &sr), FL_ERR_TIME_HORIZON);
    CHECK_EQI(fl_sender_post_event(&S, 99, FL_EV_RAISE, 1, 1, 1, evp, &id), FL_ERR_TIME_REGRESS);
    CHECK_EQ(S.stats.events_generated, 0);
    CHECK_EQI(fl_sender_step(&S, FL_TIME_HORIZON_MS, &sr), FL_OK); /* the horizon itself is valid */
    CHECK_EQ(S.stats.steps, 1);
    good_state_frame(frame, &flen);
    CHECK_EQI(fl_sender_ingest_ack(&S, 5, frame, flen, &note), FL_ERR_TIME_REGRESS);
    CHECK_EQ(S.stats.acks_rejected_frame, 0);

    CHECK_EQI(fl_receiver_init(&R, 7, 1), FL_OK);
    CHECK_EQI(fl_receiver_ingest(&R, FL_TIME_HORIZON_MS + 1u, frame, flen, &rr), FL_ERR_TIME_HORIZON);
    CHECK_EQ(R.stats.frames_ok, 0);
    CHECK_EQI(fl_receiver_ingest(&R, 50, frame, flen, &rr), FL_OK);
    CHECK_EQI(fl_receiver_ingest(&R, 49, frame, flen, &rr), FL_ERR_TIME_REGRESS);
    CHECK_EQI(fl_receiver_step(&R, 49, ack, (uint8_t)FL_MAX_FRAME_LEN, &alen), FL_ERR_TIME_REGRESS);
    CHECK_EQ(alen, 0);
    CHECK_EQ(R.stats.steps, 0);
    CHECK_EQI(fl_receiver_step(&R, 50, ack, (uint8_t)FL_MAX_FRAME_LEN, &alen), FL_OK);
    CHECK_EQ(alen, 14);
}

/* gen + rel at the extreme: (2^31-1) + 2^30 = 0xBFFFFFFF, no wrap. */
static void test_rel_range_boundaries(void)
{
    uint8_t evp[FL_EVENT_PAYLOAD_LEN] = {0};
    uint32_t id = 0;
    sender_reset(1);
    CHECK_EQI(fl_sender_post_event(&S, FL_TIME_HORIZON_MS, FL_EV_RAISE, 1, FL_MAX_REL_MS, FL_MAX_REL_MS, evp, &id), FL_OK);
    CHECK_EQ(id, 1);
    CHECK_EQ(S.events[0].retention_abs, 0xBFFFFFFFu);
    CHECK_EQ(S.events[0].deadline_abs, 0xBFFFFFFFu);
    CHECK_EQI(fl_sender_post_event(&S, FL_TIME_HORIZON_MS, FL_EV_RAISE, 1, 0, FL_MAX_REL_MS + 1u, evp, &id), FL_ERR_REL_RANGE);
    CHECK_EQ(S.stats.events_generated, 1);
    CHECK_EQI(fl_sender_post_event(&S, FL_TIME_HORIZON_MS, FL_EV_RAISE, 1, 11, 10, evp, &id), FL_ERR_REL_RANGE);
    CHECK_EQ(S.stats.events_generated, 1);
    CHECK_EQI(fl_sender_post_event(&S, FL_TIME_HORIZON_MS, FL_EV_RAISE, 1, 10, 10, evp, &id), FL_OK);
    CHECK_EQ(id, 2);
    CHECK_EQI(fl_sender_post_event(&S, FL_TIME_HORIZON_MS, FL_EV_RAISE, 1, 0, 0, evp, &id), FL_OK);
    CHECK_EQ(id, 3);
    CHECK_EQI(fl_sender_post_event(&S, FL_TIME_HORIZON_MS, 3, 1, 0, 0, evp, &id), FL_ERR_ARG); /* bad kind */
    CHECK_EQ(S.stats.events_generated, 3);
}

static void test_seq_and_id_exhaustion(void)
{
    uint8_t payload[FL_STATE_PAYLOAD_LEN] = {0};
    uint8_t evp[FL_EVENT_PAYLOAD_LEN] = {0};
    uint32_t seq = 0;
    uint32_t id = 0;
    sender_reset(1);
    S.streams[0].next_seq = UINT32_MAX;
    CHECK_EQI(fl_sender_publish_state(&S, 1, 0, payload, &seq), FL_ERR_SEQ_EXHAUSTED);
    CHECK_EQ(S.streams[0].next_seq, UINT32_MAX);
    CHECK_EQ(S.stats.state_published, 0);
    S.streams[0].next_seq = UINT32_MAX - 1u;
    CHECK_EQI(fl_sender_publish_state(&S, 1, 0, payload, &seq), FL_OK);
    CHECK_EQ(seq, UINT32_MAX - 1u);
    CHECK_EQI(fl_sender_publish_state(&S, 1, 0, payload, &seq), FL_ERR_SEQ_EXHAUSTED);

    sender_reset(1);
    S.stats.events_generated = UINT32_MAX - 1u;
    CHECK_EQI(fl_sender_post_event(&S, 1, FL_EV_RAISE, 1, 1, 1, evp, &id), FL_ERR_ID_EXHAUSTED);
    CHECK_EQ(S.stats.events_generated, UINT32_MAX - 1u);
    CHECK_EQ(S.stats.events_admitted, 0);
    S.stats.events_generated = UINT32_MAX - 2u;
    CHECK_EQI(fl_sender_post_event(&S, 1, FL_EV_RAISE, 1, 1, 1, evp, &id), FL_OK);
    CHECK_EQ(id, UINT32_MAX - 1u);
    CHECK_EQI(fl_sender_post_event(&S, 1, FL_EV_RAISE, 1, 1, 1, evp, &id), FL_ERR_ID_EXHAUSTED);
}

static void test_init_rejections(void)
{
    fl_sender_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.session_id = 7;
    cfg.n_streams = 0;
    cfg.ack_timeout_ms = 30;
    cfg.max_attempts = 3;
    CHECK_EQI(fl_sender_init(&S, &cfg), FL_ERR_ARG);
    cfg.n_streams = (uint8_t)(FL_MAX_STREAMS + 1u);
    CHECK_EQI(fl_sender_init(&S, &cfg), FL_ERR_ARG);
    cfg.n_streams = (uint8_t)FL_MAX_STREAMS;
    CHECK_EQI(fl_sender_init(&S, &cfg), FL_OK);
    cfg.ack_timeout_ms = 0;
    CHECK_EQI(fl_sender_init(&S, &cfg), FL_ERR_ARG);
    cfg.ack_timeout_ms = FL_MAX_REL_MS + 1u;
    CHECK_EQI(fl_sender_init(&S, &cfg), FL_ERR_ARG);
    cfg.ack_timeout_ms = FL_MAX_REL_MS;
    CHECK_EQI(fl_sender_init(&S, &cfg), FL_OK);
    cfg.max_attempts = 0;
    CHECK_EQI(fl_sender_init(&S, &cfg), FL_ERR_ARG);
    cfg.max_attempts = 1;
    cfg.policy.family = 4;
    CHECK_EQI(fl_sender_init(&S, &cfg), FL_ERR_ARG);
    cfg.policy.family = FL_POL_FIFO;
    cfg.policy.state_stale_ms = FL_MAX_REL_MS + 1u;
    CHECK_EQI(fl_sender_init(&S, &cfg), FL_ERR_ARG);
    cfg.policy.state_stale_ms = 0;
    CHECK_EQI(fl_sender_init(&S, &cfg), FL_OK);
    CHECK_EQI(fl_sender_init(0, &cfg), FL_ERR_ARG);
    CHECK_EQI(fl_receiver_init(&R, 7, 0), FL_ERR_ARG);
    CHECK_EQI(fl_receiver_init(&R, 7, (uint8_t)(FL_MAX_STREAMS + 1u)), FL_ERR_ARG);
    CHECK_EQI(fl_receiver_init(&R, 7, (uint8_t)FL_MAX_STREAMS), FL_OK);
    CHECK_EQI(fl_receiver_init(0, 7, 1), FL_ERR_ARG);
}

int main(void)
{
    RUN(test_sizes);
    RUN(test_round_trip_state_le);
    RUN(test_round_trip_event);
    RUN(test_round_trip_acks);
    RUN(test_decode_rejections);
    RUN(test_decode_short_buffer_canary);
    RUN(test_time_horizon_and_regress);
    RUN(test_rel_range_boundaries);
    RUN(test_seq_and_id_exhaustion);
    RUN(test_init_rejections);
    return FLTEST_REPORT("test_frame_time");
}
