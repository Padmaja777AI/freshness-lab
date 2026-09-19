/*
 * Event ledger semantics (docs/DESIGN.md §4.1, §6.3, §6.5): identity,
 * accounting, retry limit, retention, deadline vs retention, ordering,
 * ACK validation rules 1-4, ACK_STATE impossibility. Hand-computed times.
 * SPDX-License-Identifier: MIT
 */
#include "fltest.h"
#include "../core/fl_sender.h"
#include "../core/fl_frame.h"

static fl_sender_t S;
static const uint8_t P8[FL_EVENT_PAYLOAD_LEN] = {1, 2, 3, 4, 5, 6, 7, 8};

static void reset(fl_time_t ack_timeout, uint8_t max_attempts)
{
    fl_sender_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.session_id = 7;
    cfg.n_streams = 1;
    cfg.policy.family = FL_POL_EDF_RR;
    cfg.ack_timeout_ms = ack_timeout;
    cfg.max_attempts = max_attempts;
    CHECK_EQI(fl_sender_init(&S, &cfg), FL_OK);
}

static uint32_t post(fl_time_t now, fl_time_t dl, fl_time_t ret, int expect_rc)
{
    uint32_t id = 0;
    CHECK_EQI(fl_sender_post_event(&S, now, FL_EV_RAISE, 9, dl, ret, P8, &id), expect_rc);
    return id;
}

static fl_step_result_t step(fl_time_t now)
{
    fl_step_result_t r;
    CHECK_EQI(fl_sender_step(&S, now, &r), FL_OK);
    return r;
}

static uint8_t mk_ack_event(uint32_t id, uint16_t session, uint8_t *buf)
{
    fl_frame_t f;
    uint8_t len = 0;
    memset(&f, 0, sizeof(f));
    f.type = FL_FT_ACK_EVENT;
    f.session = session;
    f.u.ack_event.id = id;
    CHECK_EQI(fl_frame_encode(&f, buf, (uint8_t)FL_MAX_FRAME_LEN, &len), FL_OK);
    return len;
}

static uint8_t mk_ack_state(uint8_t stream, uint32_t seq, fl_time_t gen, uint8_t *buf)
{
    fl_frame_t f;
    uint8_t len = 0;
    memset(&f, 0, sizeof(f));
    f.type = FL_FT_ACK_STATE;
    f.session = 7;
    f.u.ack_state.stream = stream;
    f.u.ack_state.applied_seq = seq;
    f.u.ack_state.applied_gen = gen;
    CHECK_EQI(fl_frame_encode(&f, buf, (uint8_t)FL_MAX_FRAME_LEN, &len), FL_OK);
    return len;
}

static int ack_event(fl_time_t now, uint32_t id, fl_terminal_note_t *note)
{
    uint8_t buf[FL_MAX_FRAME_LEN];
    uint8_t len = mk_ack_event(id, 7, buf);
    return fl_sender_ingest_ack(&S, now, buf, len, note);
}

static int ack_state(fl_time_t now, uint32_t seq, fl_time_t gen, fl_terminal_note_t *note)
{
    uint8_t buf[FL_MAX_FRAME_LEN];
    uint8_t len = mk_ack_state(0, seq, gen, buf);
    return fl_sender_ingest_ack(&S, now, buf, len, note);
}

/* (a) identity: post at 5 (dl 100 -> 105, ret 500 -> 505); the frame and the slot never change. */
static void test_immutable_identity(void)
{
    fl_step_result_t r;
    fl_frame_t f;
    uint8_t sp[FL_STATE_PAYLOAD_LEN] = {0};
    reset(30, 3);
    CHECK_EQ(post(5, 100, 500, FL_OK), 1);
    r = step(10);
    CHECK_EQ(r.frame_len, 27);
    CHECK_EQI(fl_frame_decode(r.frame, r.frame_len, 7, &f), FL_OK);
    CHECK_EQ(f.type, FL_FT_EVENT);
    CHECK_EQ(f.u.event.id, 1);
    CHECK_EQ(f.u.event.kind, FL_EV_RAISE);
    CHECK_EQ(f.u.event.code, 9);
    CHECK_EQ(f.u.event.gen_time, 5);
    CHECK_EQ(f.u.event.deadline_abs, 105);
    CHECK(memcmp(f.u.event.payload, P8, 8) == 0);
    CHECK_EQI(fl_sender_publish_state(&S, 11, 0, sp, 0), FL_OK);
    r = step(20); /* event in flight -> state goes */
    CHECK_EQ(r.choice.kind, FL_DK_STATE);
    CHECK_EQ(S.events[0].id, 1);
    CHECK_EQ(S.events[0].gen_time, 5);
    CHECK_EQ(S.events[0].deadline_abs, 105);
    CHECK_EQ(S.events[0].retention_abs, 505);
    CHECK_EQ(S.events[0].code, 9);
    CHECK(memcmp(S.events[0].payload, P8, 8) == 0);
    CHECK_EQ(S.events[0].attempts, 1);
    CHECK_EQ(S.events[0].sent_time, 10);
}

/* (b) generated = admitted + rejected; the rejected ID is burned. */
static void test_accounting_and_burned_id(void)
{
    uint8_t i;
    fl_terminal_note_t note;
    reset(30, 3);
    for (i = 0; i < FL_EVENT_CAPACITY; i++) {
        CHECK_EQ(post(0, 100, 500, FL_OK), (uint32_t)i + 1u);
    }
    CHECK_EQ(post(0, 100, 500, FL_ERR_EVENT_FULL), FL_EVENT_CAPACITY + 1u);
    CHECK_EQ(S.stats.events_generated, FL_EVENT_CAPACITY + 1u);
    CHECK_EQ(S.stats.events_admitted, FL_EVENT_CAPACITY);
    CHECK_EQ(S.stats.events_rejected_full, 1);
    CHECK_EQ(fl_sender_pending_events(&S), FL_EVENT_CAPACITY);
    /* send id 1 (all deadlines equal -> lowest id), ACK it, next post gets id capacity+2 */
    CHECK_EQ(step(0).choice.event_id, 1);
    CHECK_EQI(ack_event(1, 1, &note), FL_OK);
    CHECK_EQ(note.outcome, FL_TERM_ACKED);
    CHECK_EQ(post(1, 100, 500, FL_OK), FL_EVENT_CAPACITY + 2u);
    CHECK_EQ(S.stats.events_generated, FL_EVENT_CAPACITY + 2u);
    CHECK_EQ(S.stats.events_generated,
             S.stats.events_admitted + S.stats.events_rejected_full);
}

/* (c) retry limit: max 2, timeout 30: sends at 0 and 30, exhausted at 60. */
static void test_retry_limit(void)
{
    fl_step_result_t r;
    fl_terminal_note_t note;
    reset(30, 2);
    post(0, 1000, 1000, FL_OK);
    r = step(0);
    CHECK_EQ(r.frame_len, 27);
    CHECK_EQ(r.attempt, 1);
    r = step(10);
    CHECK_EQ(r.frame_len, 0);
    r = step(20);
    CHECK_EQ(r.frame_len, 0);
    r = step(30); /* 30 - 0 >= 30: timeout is inclusive */
    CHECK_EQ(r.frame_len, 27);
    CHECK_EQ(r.attempt, 2);
    CHECK_EQ(r.n_terminal, 0);
    r = step(59);
    CHECK_EQ(r.frame_len, 0);
    CHECK_EQ(r.n_terminal, 0);
    r = step(60);
    CHECK_EQ(r.frame_len, 0);
    CHECK_EQ(r.n_terminal, 1);
    CHECK_EQ(r.terminal[0].id, 1);
    CHECK_EQ(r.terminal[0].outcome, FL_TERM_RETRY_EXHAUSTED);
    CHECK_EQ(r.terminal[0].terminal_time, 60);
    CHECK_EQ(r.terminal[0].attempts, 2);
    CHECK_EQ(S.stats.events_retry_exhausted, 1);
    CHECK_EQ(S.stats.event_tx, 2);
    CHECK_EQ(S.stats.event_first_tx, 1);
    CHECK_EQ(fl_sender_pending_events(&S), 0);
    CHECK_EQI(ack_event(61, 1, &note), FL_ERR_ACK_UNMATCHED);
    CHECK_EQ(S.stats.acks_unmatched, 1);
    CHECK_EQ(S.stats.events_acked, 0);
    CHECK_EQ(note.outcome, FL_TERM_PENDING);
}

/* (d) retention boundary: expires at now == retention_abs, even in flight. */
static void test_retention_boundary(void)
{
    fl_step_result_t r;
    reset(1000, 3);
    post(0, 50, 50, FL_OK);
    r = step(0);
    CHECK_EQ(r.frame_len, 27);
    r = step(49);
    CHECK_EQ(r.n_terminal, 0);
    CHECK_EQ(fl_sender_pending_events(&S), 1);
    r = step(50);
    CHECK_EQ(r.n_terminal, 1);
    CHECK_EQ(r.terminal[0].outcome, FL_TERM_RETENTION_EXPIRED);
    CHECK_EQ(r.terminal[0].terminal_time, 50);
    CHECK_EQ(r.terminal[0].attempts, 1);
    CHECK_EQ(S.stats.events_retention_expired, 1);
    CHECK_EQ(fl_sender_pending_events(&S), 0);
    r = step(60);
    CHECK_EQ(r.n_terminal, 0);
    /* retention_rel 0: expires at the first step at or after generation, never sent */
    post(70, 0, 0, FL_OK);
    r = step(70);
    CHECK_EQ(r.frame_len, 0);
    CHECK_EQ(r.n_terminal, 1);
    CHECK_EQ(r.terminal[0].attempts, 0);
    CHECK_EQ(r.terminal[0].outcome, FL_TERM_RETENTION_EXPIRED);
}

/* (e) deadline passes, retention does not: still transmitted, reason EVENT_LATE. */
static void test_deadline_distinct_from_retention(void)
{
    fl_step_result_t r;
    reset(30, 3);
    post(0, 20, 100, FL_OK);
    r = step(0);
    CHECK_EQ(r.choice.reason, FL_R_EVENT_EDF);
    r = step(30);
    CHECK_EQ(r.frame_len, 27);
    CHECK_EQ(r.attempt, 2);
    CHECK_EQ(r.choice.reason, FL_R_EVENT_LATE);
    CHECK_EQ(r.n_terminal, 0);
    CHECK_EQ(fl_sender_pending_events(&S), 1);
}

/* (f) expiry and exhaustion in one step -> RETENTION_EXPIRED only. */
static void test_expiry_beats_exhaustion(void)
{
    fl_step_result_t r;
    reset(10, 1);
    post(0, 10, 10, FL_OK);
    r = step(0);
    CHECK_EQ(r.frame_len, 27);
    r = step(10);
    CHECK_EQ(r.n_terminal, 1);
    CHECK_EQ(r.terminal[0].outcome, FL_TERM_RETENTION_EXPIRED);
    CHECK_EQ(S.stats.events_retention_expired, 1);
    CHECK_EQ(S.stats.events_retry_exhausted, 0);
}

/* (g)(h)(i) ordering. */
static void test_ordering(void)
{
    fl_step_result_t r;
    /* same time, same deadline -> id 1 then id 2 */
    reset(30, 3);
    post(0, 100, 500, FL_OK);
    post(0, 100, 500, FL_OK);
    r = step(0);
    CHECK_EQ(r.choice.event_id, 1);
    r = step(10);
    CHECK_EQ(r.choice.event_id, 2);
    /* deadline tie with different gen: A(0,+100)=100, B(10,+90)=100 -> id 1 */
    reset(30, 3);
    post(0, 100, 500, FL_OK);
    post(10, 90, 500, FL_OK);
    r = step(10);
    CHECK_EQ(r.choice.event_id, 1);
    /* EDF proper: A(0,+500), B(10,+50)=60 -> id 2 first */
    reset(30, 3);
    post(0, 500, 500, FL_OK);
    post(10, 50, 500, FL_OK);
    r = step(10);
    CHECK_EQ(r.choice.event_id, 2);
    CHECK_EQ(r.choice.n_elig_ev, 2);
}

/* (j)(l) ACK_EVENT validation rules 1-4 and confirmation time. */
static void test_ack_event_validation(void)
{
    fl_terminal_note_t note;
    uint8_t buf[FL_MAX_FRAME_LEN];
    uint8_t len;
    uint8_t i;
    reset(30, 3);
    CHECK_EQI(ack_event(0, 0, &note), FL_ERR_ACK_IMPOSSIBLE);
    CHECK_EQI(ack_event(0, 1, &note), FL_ERR_ACK_IMPOSSIBLE); /* nothing generated yet */
    CHECK_EQ(S.stats.acks_impossible, 2);
    post(0, 100, 500, FL_OK); /* id 1, never sent */
    CHECK_EQI(ack_event(1, 1, &note), FL_ERR_ACK_IMPOSSIBLE);
    CHECK_EQ(fl_sender_pending_events(&S), 1);
    CHECK_EQ(S.stats.events_acked, 0);
    CHECK_EQ(note.outcome, FL_TERM_PENDING);
    for (i = 1; i < FL_EVENT_CAPACITY; i++) {
        post(1, 100, 500, FL_OK); /* ids 2..capacity */
    }
    CHECK_EQ(post(1, 100, 500, FL_ERR_EVENT_FULL), FL_EVENT_CAPACITY + 1u);
    CHECK_EQI(ack_event(2, FL_EVENT_CAPACITY + 1u, &note), FL_ERR_ACK_UNMATCHED); /* rejected id */
    CHECK_EQI(ack_event(2, FL_EVENT_CAPACITY + 2u, &note), FL_ERR_ACK_IMPOSSIBLE); /* beyond generated */
    CHECK_EQ(step(2).choice.event_id, 1);
    CHECK_EQI(ack_event(9, 1, &note), FL_OK);
    CHECK_EQ(note.outcome, FL_TERM_ACKED);
    CHECK_EQ(note.id, 1);
    CHECK_EQ(note.terminal_time, 9); /* confirmation time = ACK arrival, gen was 0 */
    CHECK_EQ(note.attempts, 1);
    CHECK_EQ(S.stats.events_acked, 1);
    CHECK_EQI(ack_event(10, 1, &note), FL_ERR_ACK_UNMATCHED); /* already terminal */
    CHECK_EQ(S.stats.events_acked, 1);
    /* wrong frame type and wrong session */
    {
        fl_frame_t f;
        memset(&f, 0, sizeof(f));
        f.type = FL_FT_STATE;
        f.session = 7;
        CHECK_EQI(fl_frame_encode(&f, buf, (uint8_t)FL_MAX_FRAME_LEN, &len), FL_OK);
        CHECK_EQI(fl_sender_ingest_ack(&S, 11, buf, len, &note), FL_ERR_FRAME_TYPE);
        CHECK_EQ(S.stats.acks_rejected_frame, 1);
    }
    len = mk_ack_event(2, 8, buf);
    CHECK_EQI(fl_sender_ingest_ack(&S, 12, buf, len, &note), FL_ERR_FRAME_SESSION);
    CHECK_EQ(S.stats.acks_session_mismatch, 1);
    len = mk_ack_event(2, 7, buf);
    CHECK_EQI(fl_sender_ingest_ack(&S, 12, buf, 9, &note), FL_ERR_FRAME_LEN);
    CHECK_EQ(S.stats.acks_rejected_frame, 2);
    CHECK_EQ(S.stats.acks_impossible, 4);
    CHECK_EQ(S.stats.acks_unmatched, 2);
}

/* (k) ACK_STATE impossibility: beyond max_sent, gen ahead of what was sent. */
static void test_ack_state_validation(void)
{
    fl_terminal_note_t note;
    uint8_t sp[FL_STATE_PAYLOAD_LEN] = {0};
    fl_step_result_t r;
    reset(30, 3);
    CHECK_EQI(ack_state(5, 1, 0, &note), FL_ERR_ACK_IMPOSSIBLE); /* nothing sent */
    CHECK_EQ(S.streams[0].last_acked_seq, 0);
    CHECK_EQI(fl_sender_publish_state(&S, 5, 0, sp, 0), FL_OK);
    r = step(5);
    CHECK_EQ(r.choice.kind, FL_DK_STATE);
    CHECK_EQ(S.streams[0].max_sent_seq, 1);
    CHECK_EQ(S.streams[0].max_sent_gen, 5);
    CHECK_EQI(ack_state(10, 2, 5, &note), FL_ERR_ACK_IMPOSSIBLE);  /* seq beyond max_sent */
    CHECK_EQI(ack_state(10, 1, 6, &note), FL_ERR_ACK_IMPOSSIBLE);  /* gen beyond max_sent_gen */
    CHECK_EQ(S.streams[0].last_acked_seq, 0);
    CHECK_EQ(S.streams[0].inflight_valid, 1);
    CHECK_EQI(ack_state(10, 1, 5, &note), FL_OK);
    CHECK_EQ(S.streams[0].last_acked_seq, 1);
    CHECK_EQ(S.streams[0].last_acked_gen, 5);
    CHECK_EQ(S.streams[0].inflight_valid, 0);
    CHECK_EQI(ack_state(11, 1, 5, &note), FL_OK); /* repeat is harmless */
    CHECK_EQI(ack_state(11, 1, 4, &note), FL_ERR_ACK_IMPOSSIBLE); /* same seq, different gen */
    CHECK_EQ(S.stats.acks_impossible, 4);
    CHECK_EQ(S.stats.acks_ok, 2);
    /* stream index beyond n_streams is a field error, counted as a rejected frame */
    {
        uint8_t buf[FL_MAX_FRAME_LEN];
        uint8_t len = mk_ack_state(1, 1, 5, buf);
        CHECK_EQI(fl_sender_ingest_ack(&S, 12, buf, len, &note), FL_ERR_FRAME_FIELD);
        CHECK_EQ(S.stats.acks_rejected_frame, 1);
    }
}

/* (m) housekeeping happens only in step: an expired event stays 'used' until then. */
static void test_housekeeping_only_in_step(void)
{
    fl_step_result_t r;
    reset(30, 3);
    post(0, 10, 10, FL_OK);
    CHECK_EQ(fl_sender_pending_events(&S), 1);
    CHECK_EQI(fl_sender_publish_state(&S, 20, 0, P8, 0), FL_OK); /* time moves, no step */
    CHECK_EQ(fl_sender_pending_events(&S), 1);
    r = step(20);
    CHECK_EQ(r.n_terminal, 1);
    CHECK_EQ(r.terminal[0].terminal_time, 20);
    CHECK_EQ(fl_sender_pending_events(&S), 0);
}

/* Terminal notes are bounded by capacity: fill all slots, expire all in one step. */
static void test_terminal_note_capacity(void)
{
    fl_step_result_t r;
    uint8_t i;
    reset(30, 3);
    for (i = 0; i < FL_EVENT_CAPACITY; i++) {
        post(0, 5, 5, FL_OK);
    }
    r = step(5);
    CHECK_EQ(r.n_terminal, FL_EVENT_CAPACITY);
    CHECK_EQ(S.stats.events_retention_expired, FL_EVENT_CAPACITY);
    CHECK_EQ(fl_sender_pending_events(&S), 0);
    CHECK_EQ(r.frame_len, 0);
}

int main(void)
{
    RUN(test_immutable_identity);
    RUN(test_accounting_and_burned_id);
    RUN(test_retry_limit);
    RUN(test_retention_boundary);
    RUN(test_deadline_distinct_from_retention);
    RUN(test_expiry_beats_exhaustion);
    RUN(test_ordering);
    RUN(test_ack_event_validation);
    RUN(test_ack_state_validation);
    RUN(test_housekeeping_only_in_step);
    RUN(test_terminal_note_capacity);
    return FLTEST_REPORT("test_events");
}
