/*
 * Freshness Lab — transmitter core (docs/DESIGN.md §6).
 * Fixed memory, no heap, no I/O. Host supplies `now` explicitly.
 * SPDX-License-Identifier: MIT
 */
#ifndef FL_SENDER_H
#define FL_SENDER_H

#include "fl_types.h"
#include "fl_policy.h"

typedef struct {
    uint16_t session_id;
    uint8_t n_streams;          /* 1..FL_MAX_STREAMS */
    fl_policy_params_t policy;
    fl_time_t ack_timeout_ms;   /* 1..FL_MAX_REL_MS */
    uint8_t max_attempts;       /* >= 1 */
} fl_sender_config_t;

typedef struct {
    uint32_t id;
    uint8_t outcome;            /* enum fl_terminal */
    fl_time_t terminal_time;
    uint8_t attempts;
} fl_terminal_note_t;

typedef struct {
    uint8_t frame_len;          /* 0 = nothing to send */
    uint8_t frame[FL_MAX_FRAME_LEN];
    fl_choice_t choice;
    uint8_t attempt;            /* attempt number of the event sent (1 = first) */
    uint32_t seq;               /* seq of the state sent */
    uint8_t n_terminal;
    fl_terminal_note_t terminal[FL_EVENT_CAPACITY];
    uint8_t state_timeouts;     /* in-flight state frames timed out this step */
} fl_step_result_t;

typedef struct {
    uint32_t events_generated;
    uint32_t events_admitted;
    uint32_t events_rejected_full;
    uint32_t events_acked;
    uint32_t events_retry_exhausted;
    uint32_t events_retention_expired;
    uint32_t event_tx;          /* every event transmission incl. retries */
    uint32_t event_first_tx;    /* first attempts only */
    uint32_t state_published;
    uint32_t state_superseded;  /* waiting snapshot replaced before tx */
    uint32_t state_tx;
    uint32_t state_ack_timeouts;
    uint32_t acks_ok;
    uint32_t acks_unmatched;
    uint32_t acks_impossible;
    uint32_t acks_rejected_frame;
    uint32_t acks_session_mismatch;
    uint32_t steps;
    uint64_t bytes_data_tx;
} fl_sender_stats_t;

typedef struct {
    uint8_t latest_valid;
    uint32_t latest_seq;
    fl_time_t latest_gen;
    uint8_t latest_payload[FL_STATE_PAYLOAD_LEN];
    uint8_t inflight_valid;
    uint32_t inflight_seq;
    fl_time_t inflight_gen;
    fl_time_t inflight_sent;
    uint32_t last_acked_seq;    /* 0 = none */
    fl_time_t last_acked_gen;   /* FL_TIME_NONE = none */
    uint32_t max_sent_seq;      /* 0 = none */
    fl_time_t max_sent_gen;     /* FL_TIME_NONE = none */
    fl_time_t last_tx_time;     /* FL_TIME_NONE = never */
    uint32_t next_seq;
} fl_sender_stream_t;

typedef struct {
    uint8_t used;
    uint8_t kind;
    uint8_t code;
    uint8_t attempts;
    uint8_t in_flight;
    uint32_t id;
    fl_time_t gen_time;
    fl_time_t deadline_abs;
    fl_time_t retention_abs;
    fl_time_t sent_time;
    uint8_t payload[FL_EVENT_PAYLOAD_LEN];
} fl_sender_event_t;

typedef struct {
    fl_sender_config_t cfg;
    fl_time_t last_now;         /* FL_TIME_NONE before first call */
    uint8_t rr_next;
    fl_sender_stream_t streams[FL_MAX_STREAMS];
    fl_sender_event_t events[FL_EVENT_CAPACITY];
    fl_sender_stats_t stats;
} fl_sender_t;

int fl_sender_init(fl_sender_t *s, const fl_sender_config_t *cfg);

/* Publish a STATE snapshot; supersedes any waiting snapshot on the stream. */
int fl_sender_publish_state(fl_sender_t *s, fl_time_t now, uint8_t stream,
                            const uint8_t payload[FL_STATE_PAYLOAD_LEN], uint32_t *seq_out);

/*
 * Generate an EVENT occurrence. Always assigns an ID (reported via id_out)
 * unless the arguments are invalid or IDs are exhausted. Returns FL_OK when
 * admitted, FL_ERR_EVENT_FULL when rejected (ID burned, counted).
 */
int fl_sender_post_event(fl_sender_t *s, fl_time_t now, uint8_t kind, uint8_t code,
                         fl_time_t deadline_rel, fl_time_t retention_rel,
                         const uint8_t payload[FL_EVENT_PAYLOAD_LEN], uint32_t *id_out);

/* One transmit opportunity: housekeeping, then at most one frame. */
int fl_sender_step(fl_sender_t *s, fl_time_t now, fl_step_result_t *r);

/*
 * Ingest an ACK frame. On FL_OK with an event ACK, *note is filled
 * (note->outcome == FL_TERM_ACKED); otherwise note->outcome == FL_TERM_PENDING.
 * Returns FL_ERR_ACK_IMPOSSIBLE / FL_ERR_ACK_UNMATCHED / FL_ERR_FRAME_* on
 * rejected ACKs (all counted).
 */
int fl_sender_ingest_ack(fl_sender_t *s, fl_time_t now, const uint8_t *bytes, uint8_t len,
                         fl_terminal_note_t *note);

/* Number of used (pending) event slots. */
uint8_t fl_sender_pending_events(const fl_sender_t *s);

/* Builds the policy view from the sender's own ledger (exposed for tests). */
void fl_sender_build_view(const fl_sender_t *s, fl_time_t now, fl_sched_view_t *v);

#endif /* FL_SENDER_H */
