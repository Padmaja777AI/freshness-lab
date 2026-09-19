/*
 * Hand-computed fixtures from docs/DESIGN.md §8.6 and §12:
 *  - continuous-time AoI fixture (area 28, mean 2.8, peak 6, final age 4)
 *  - lost-ACK ledger fixture (independent sender/receiver ledgers, lost
 *    attempts charged to the byte budget)
 * These are specifications the code must meet, not values copied from a run.
 * SPDX-License-Identifier: MIT
 */
#include "fltest.h"
#include "../core/fl_sender.h"
#include "../core/fl_receiver.h"
#include "../core/fl_frame.h"
#include "../host/aoi.h"

/*
 * Samples (gen, rx) = (0,0), (2,3), (6,8), run end 10.
 * Segment [0,3): age 0 -> 3, area 4.5 (2*area = 9), end age 3.
 * Segment [3,8): age 1 -> 6, area 17.5 (35), end age 6.
 * Segment [8,10): age 2 -> 4, area 6 (12), end age 4.
 * Total 2*area = 56 -> area 28, mean 28/10 = 2.8, peak 6, final age 4.
 * Threshold 3: {t: age > 3} = (6-3) + (4-3) = 4 (first segment ends at exactly 3).
 * A discrete left-sample sum would give 23 (mean 2.3): that must NOT be what we compute.
 */
static void test_aoi_fixture(void)
{
    aoi_acc_t a;
    aoi_init(&a, 3);
    aoi_apply(&a, 0, 0);
    aoi_apply(&a, 3, 2);
    aoi_apply(&a, 8, 6);
    aoi_finish(&a, 10);
    CHECK_EQ(a.area2, 56);
    CHECK_NEAR(aoi_area(&a), 28.0, 1e-9);
    CHECK_NEAR(aoi_mean(&a, 10), 2.8, 1e-9);
    CHECK(aoi_area(&a) != 23.0);
    CHECK_EQ(a.peak, 6);
    CHECK_EQ(a.final_age, 4);
    CHECK_EQ(a.unknown_ms, 0);
    CHECK_EQ(a.over_ms, 4);
    CHECK_EQ(a.first_apply, 0);
}

/* First apply at t=5 (gen 5): unknown 5, then one segment [5,10) age 0->5: area 12.5, mean 12.5/5 = 2.5. */
static void test_aoi_warmup_and_never(void)
{
    aoi_acc_t a;
    aoi_acc_t b;
    aoi_init(&a, 100);
    aoi_apply(&a, 5, 5);
    aoi_finish(&a, 10);
    CHECK_EQ(a.unknown_ms, 5);
    CHECK_EQ(a.area2, 25);
    CHECK_NEAR(aoi_mean(&a, 10), 2.5, 1e-9);
    CHECK_EQ(a.peak, 5);
    CHECK_EQ(a.over_ms, 0);
    aoi_init(&b, 1);
    aoi_finish(&b, 10);
    CHECK_EQ(b.defined, 0);
    CHECK_EQ(b.unknown_ms, 10);
    CHECK_NEAR(aoi_mean(&b, 10), 0.0, 1e-12);
}

/* Threshold exactly at the start age: [0,4) age 2->6 with threshold 2 -> all 4 ms count (a0 >= theta). */
static void test_aoi_threshold_boundary(void)
{
    aoi_acc_t a;
    aoi_init(&a, 2);
    aoi_apply(&a, 2, 0); /* first apply at t=2 of a snapshot generated at 0: age starts at 2 */
    aoi_finish(&a, 6);
    CHECK_EQ(a.over_ms, 4);
    CHECK_EQ(a.area2, 2u * 4u * 2u + 16u); /* 2*n*a0 + n*n = 16 + 16 */
    CHECK_EQ(a.peak, 6);
}

static uint8_t encode_ack_event(uint32_t id, uint8_t flags, uint8_t *buf)
{
    fl_frame_t f;
    uint8_t len = 0;
    f.type = FL_FT_ACK_EVENT;
    f.session = 7;
    f.u.ack_event.id = id;
    f.u.ack_event.flags = flags;
    CHECK_EQI(fl_frame_encode(&f, buf, (uint8_t)FL_MAX_FRAME_LEN, &len), FL_OK);
    return len;
}

/*
 * Lost-ACK fixture (§12), millisecond slots driven by hand:
 *  t=0 generate E (deadline_rel 10, retention_rel 20); t=1 send (27 B);
 *  t=2 receive (on time), ACK attempted (10 B) and LOST; t=6 retry after the
 *  5 ms timeout (27 B); t=7 receive again = duplicate, ACK (10 B) sent;
 *  t=8 ACK arrives.
 *  Cutoff 5: rx unique 1 / on-time 1 / dup 0; sender pending 1, acked 0;
 *            attempted bytes 27 + 10 = 37.
 *  Cutoff 8: rx unique 1 / dup 1; sender acked 1, pending 0;
 *            attempted bytes 54 + 20 = 74; first-delivery latency 2,
 *            confirmation latency 8.
 */
static void test_lost_ack_fixture(void)
{
    fl_sender_t s;
    fl_receiver_t r;
    fl_sender_config_t cfg;
    fl_step_result_t sr;
    fl_rx_result_t rr;
    fl_terminal_note_t note;
    uint8_t payload[FL_EVENT_PAYLOAD_LEN] = {0};
    uint8_t ack[FL_MAX_FRAME_LEN];
    uint8_t alen = 0;
    uint8_t data1[FL_MAX_FRAME_LEN];
    uint8_t data1_len = 0;
    uint8_t data2[FL_MAX_FRAME_LEN];
    uint8_t data2_len = 0;
    uint32_t id = 0;
    fl_time_t t;

    memset(&cfg, 0, sizeof(cfg));
    cfg.session_id = 7;
    cfg.n_streams = 1;
    cfg.policy.family = FL_POL_EDF_RR;
    cfg.ack_timeout_ms = 5;
    cfg.max_attempts = 3;
    CHECK_EQI(fl_sender_init(&s, &cfg), FL_OK);
    CHECK_EQI(fl_receiver_init(&r, 7, 1), FL_OK);

    CHECK_EQI(fl_sender_post_event(&s, 0, FL_EV_RAISE, 1, 10, 20, payload, &id), FL_OK);
    CHECK_EQ(id, 1);

    /* t=1: transmit opportunity -> first attempt */
    CHECK_EQI(fl_sender_step(&s, 1, &sr), FL_OK);
    CHECK_EQ(sr.frame_len, 27);
    CHECK_EQ(sr.choice.kind, FL_DK_EVENT);
    CHECK_EQ(sr.attempt, 1);
    memcpy(data1, sr.frame, sr.frame_len);
    data1_len = sr.frame_len;
    CHECK_EQ(s.stats.bytes_data_tx, 27);

    /* t=2: arrives, delivered on time (2 <= 10); ACK attempted and lost */
    CHECK_EQI(fl_receiver_ingest(&r, 2, data1, data1_len, &rr), FL_OK);
    CHECK_EQ(rr.outcome, FL_RX_EVENT_DELIVERED);
    CHECK_EQ(rr.on_time, 1);
    CHECK_EQ(rr.event_id, 1);
    CHECK_EQI(fl_sender_step(&s, 2, &sr), FL_OK);
    CHECK_EQ(sr.frame_len, 0); /* in flight */
    CHECK_EQI(fl_receiver_step(&r, 2, ack, (uint8_t)FL_MAX_FRAME_LEN, &alen), FL_OK);
    CHECK_EQ(alen, 10);
    CHECK_EQ(ack[1], FL_FT_ACK_EVENT);
    CHECK_EQ(r.stats.bytes_ack_tx, 10); /* charged although lost */
    /* (the ACK is dropped by the channel: never fed to the sender) */

    for (t = 3; t <= 5; t++) {
        CHECK_EQI(fl_sender_step(&s, t, &sr), FL_OK);
        CHECK_EQ(sr.frame_len, 0);
        CHECK_EQI(fl_receiver_step(&r, t, ack, (uint8_t)FL_MAX_FRAME_LEN, &alen), FL_OK);
        CHECK_EQ(alen, 0);
    }
    /* cutoff 5 */
    CHECK_EQ(r.stats.ev_delivered, 1);
    CHECK_EQ(r.stats.ev_on_time, 1);
    CHECK_EQ(r.stats.ev_duplicate, 0);
    CHECK_EQ(fl_sender_pending_events(&s), 1);
    CHECK_EQ(s.stats.events_acked, 0);
    CHECK_EQ(s.stats.bytes_data_tx + r.stats.bytes_ack_tx, 37);

    /* t=6: timeout (6-1 >= 5) -> retry, attempt 2 */
    CHECK_EQI(fl_sender_step(&s, 6, &sr), FL_OK);
    CHECK_EQ(sr.frame_len, 27);
    CHECK_EQ(sr.attempt, 2);
    CHECK_EQ(sr.n_terminal, 0);
    memcpy(data2, sr.frame, sr.frame_len);
    data2_len = sr.frame_len;
    CHECK(memcmp(data1, data2, data1_len) == 0); /* identical immutable record */
    CHECK_EQI(fl_receiver_step(&r, 6, ack, (uint8_t)FL_MAX_FRAME_LEN, &alen), FL_OK);
    CHECK_EQ(alen, 0);

    /* t=7: duplicate at the receiver; ACK sent (this one is delivered) */
    CHECK_EQI(fl_receiver_ingest(&r, 7, data2, data2_len, &rr), FL_OK);
    CHECK_EQ(rr.outcome, FL_RX_EVENT_DUPLICATE);
    CHECK_EQI(fl_sender_step(&s, 7, &sr), FL_OK);
    CHECK_EQ(sr.frame_len, 0);
    CHECK_EQI(fl_receiver_step(&r, 7, ack, (uint8_t)FL_MAX_FRAME_LEN, &alen), FL_OK);
    CHECK_EQ(alen, 10);
    CHECK_EQ(ack[9] & FL_ACKF_DUPLICATE, FL_ACKF_DUPLICATE);

    /* t=8: ACK arrives -> ACKED with confirmation at 8 */
    CHECK_EQI(fl_sender_ingest_ack(&s, 8, ack, alen, &note), FL_OK);
    CHECK_EQ(note.outcome, FL_TERM_ACKED);
    CHECK_EQ(note.id, 1);
    CHECK_EQ(note.terminal_time, 8);
    CHECK_EQ(note.attempts, 2);
    CHECK_EQI(fl_sender_step(&s, 8, &sr), FL_OK);
    CHECK_EQ(sr.frame_len, 0);

    /* cutoff 8 */
    CHECK_EQ(r.stats.ev_delivered, 1);   /* unique application effects */
    CHECK_EQ(r.stats.ev_duplicate, 1);
    CHECK_EQ(s.stats.events_acked, 1);
    CHECK_EQ(fl_sender_pending_events(&s), 0);
    CHECK_EQ(s.stats.event_tx, 2);
    CHECK_EQ(s.stats.bytes_data_tx, 54);
    CHECK_EQ(r.stats.bytes_ack_tx, 20);
    CHECK_EQ(s.stats.bytes_data_tx + r.stats.bytes_ack_tx, 74);
    /* first-delivery latency 2 (rx 2 - gen 0) vs confirmation latency 8 (ack 8 - gen 0) */
    CHECK_EQ(rr.gen_time, 0);
    CHECK_EQ(2u - rr.gen_time, 2);
    CHECK_EQ(note.terminal_time - rr.gen_time, 8);
    /* a stray second copy of the same ACK is unmatched, never re-applied */
    {
        uint8_t again[FL_MAX_FRAME_LEN];
        uint8_t alen2 = encode_ack_event(1, 0, again);
        CHECK_EQI(fl_sender_ingest_ack(&s, 9, again, alen2, &note), FL_ERR_ACK_UNMATCHED);
        CHECK_EQ(s.stats.events_acked, 1);
        CHECK_EQ(s.stats.acks_unmatched, 1);
    }
}

/*
 * Variation: the ACK is lost every time and the event exhausts its retries.
 * max_attempts 3, ack_timeout 5, deadline 10, retention 100: sends at 1, 6, 11;
 * exhausted at 16. Receiver: 1 unique (on time at 2), 2 duplicates (7, 12).
 * delivered_but_unacked situation: receiver says delivered, sender says
 * RETRY_EXHAUSTED. Both are true; the host reports both.
 */
static void test_lost_ack_until_exhaustion(void)
{
    fl_sender_t s;
    fl_receiver_t r;
    fl_sender_config_t cfg;
    fl_step_result_t sr;
    fl_rx_result_t rr;
    uint8_t payload[FL_EVENT_PAYLOAD_LEN] = {0};
    uint8_t ack[FL_MAX_FRAME_LEN];
    uint8_t alen = 0;
    uint32_t id = 0;
    fl_time_t t;
    uint32_t sends = 0;
    memset(&cfg, 0, sizeof(cfg));
    cfg.session_id = 7;
    cfg.n_streams = 1;
    cfg.policy.family = FL_POL_EDF_RR;
    cfg.ack_timeout_ms = 5;
    cfg.max_attempts = 3;
    CHECK_EQI(fl_sender_init(&s, &cfg), FL_OK);
    CHECK_EQI(fl_receiver_init(&r, 7, 1), FL_OK);
    CHECK_EQI(fl_sender_post_event(&s, 0, FL_EV_CLEAR, 2, 10, 100, payload, &id), FL_OK);
    for (t = 1; t <= 16; t++) {
        CHECK_EQI(fl_sender_step(&s, t, &sr), FL_OK);
        if (sr.frame_len > 0) {
            sends++;
            CHECK(t == 1 || t == 6 || t == 11);
            CHECK_EQI(fl_receiver_ingest(&r, t + 1u, sr.frame, sr.frame_len, &rr), FL_OK);
            CHECK_EQI(fl_receiver_step(&r, t + 1u, ack, (uint8_t)FL_MAX_FRAME_LEN, &alen), FL_OK);
            CHECK_EQ(alen, 10); /* attempted, then lost */
        }
        if (t == 16) {
            CHECK_EQ(sr.n_terminal, 1);
            CHECK_EQ(sr.terminal[0].outcome, FL_TERM_RETRY_EXHAUSTED);
            CHECK_EQ(sr.terminal[0].terminal_time, 16);
            CHECK_EQ(sr.terminal[0].attempts, 3);
        } else {
            CHECK_EQ(sr.n_terminal, 0);
        }
    }
    CHECK_EQ(sends, 3);
    CHECK_EQ(r.stats.ev_delivered, 1);
    CHECK_EQ(r.stats.ev_on_time, 1);
    CHECK_EQ(r.stats.ev_duplicate, 2);
    CHECK_EQ(s.stats.events_retry_exhausted, 1);
    CHECK_EQ(s.stats.events_acked, 0);
    CHECK_EQ(s.stats.bytes_data_tx, 81);
    CHECK_EQ(r.stats.bytes_ack_tx, 30);
}

int main(void)
{
    RUN(test_aoi_fixture);
    RUN(test_aoi_warmup_and_never);
    RUN(test_aoi_threshold_boundary);
    RUN(test_lost_ack_fixture);
    RUN(test_lost_ack_until_exhaustion);
    return FLTEST_REPORT("test_fixtures");
}
