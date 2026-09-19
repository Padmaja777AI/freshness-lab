/*
 * Freshness Lab — receiver core (docs/DESIGN.md §7).
 * Fixed memory, no heap, no I/O. Ingest never transmits: it enqueues an ACK
 * in a bounded queue that fl_receiver_step() drains one frame per call.
 * SPDX-License-Identifier: MIT
 */
#ifndef FL_RECEIVER_H
#define FL_RECEIVER_H

#include "fl_types.h"

enum fl_rx_outcome {
    FL_RX_NONE = 0,
    FL_RX_REJECTED = 1,          /* parse/session/field rejection, see err */
    FL_RX_STATE_APPLIED = 2,
    FL_RX_STATE_STALE = 3,       /* seq <= current: dropped, ACK still queued */
    FL_RX_EVENT_DELIVERED = 4,
    FL_RX_EVENT_DUPLICATE = 5,   /* in window, already delivered */
    FL_RX_EVENT_OUT_OF_WINDOW = 6/* too old to judge: rejected, no ACK */
};

typedef struct {
    uint8_t outcome;             /* enum fl_rx_outcome */
    int err;                     /* FL_ERR_* when outcome == REJECTED */
    uint8_t stream;
    uint32_t seq;
    uint32_t event_id;
    uint8_t ev_kind;
    uint8_t ev_code;
    fl_time_t gen_time;
    fl_time_t deadline_abs;
    uint8_t on_time;             /* rx_time <= deadline_abs (inclusive) */
    uint8_t ack_dropped;         /* 1 if the ACK could not be queued */
    uint8_t payload[FL_EVENT_PAYLOAD_LEN > FL_STATE_PAYLOAD_LEN ? FL_EVENT_PAYLOAD_LEN
                                                                 : FL_STATE_PAYLOAD_LEN];
} fl_rx_result_t;

typedef struct {
    uint32_t frames_ok;
    uint32_t frames_rejected;    /* len/magic/type/field */
    uint32_t session_mismatch;
    uint32_t state_applied;
    uint32_t state_stale_dropped;
    uint32_t ev_delivered;
    uint32_t ev_on_time;
    uint32_t ev_late;
    uint32_t ev_duplicate;
    uint32_t ev_out_of_window;
    uint32_t ack_event_dropped;  /* reverse queue overflow */
    uint32_t ack_state_coalesced;
    uint32_t ack_tx;
    uint32_t steps;
    uint64_t bytes_ack_tx;
} fl_receiver_stats_t;

typedef struct {
    uint8_t valid;
    uint32_t seq;
    fl_time_t gen_time;
    uint8_t payload[FL_STATE_PAYLOAD_LEN];
} fl_receiver_stream_t;

typedef struct {
    uint32_t id;
    uint8_t flags;
} fl_ack_entry_t;

typedef struct {
    uint16_t session_id;
    uint8_t n_streams;
    fl_time_t last_now;
    fl_receiver_stream_t streams[FL_MAX_STREAMS];
    uint32_t highest_id;         /* 0 = none delivered yet */
    uint64_t dedup_bits;         /* bit k <-> id highest_id - k */
    uint8_t ack_state_pending[FL_MAX_STREAMS];
    uint8_t ack_rr_next;
    fl_ack_entry_t ack_ring[FL_RX_ACK_QUEUE];
    uint8_t ack_head;
    uint8_t ack_count;
    fl_receiver_stats_t stats;
} fl_receiver_t;

int fl_receiver_init(fl_receiver_t *r, uint16_t session_id, uint8_t n_streams);

/* Ingest one DATA frame; fills *out; queues an ACK where §7 says so. */
int fl_receiver_ingest(fl_receiver_t *r, fl_time_t now, const uint8_t *bytes, uint8_t len,
                       fl_rx_result_t *out);

/* One reverse transmit opportunity: emits at most one ACK frame (len 0 = none). */
int fl_receiver_step(fl_receiver_t *r, fl_time_t now, uint8_t *ack_out, uint8_t cap, uint8_t *len_out);

/* Current applied snapshot (valid == 0 if none). */
const fl_receiver_stream_t *fl_receiver_state(const fl_receiver_t *r, uint8_t stream);

/* Queue occupancy helpers for tests. */
uint8_t fl_receiver_ack_queue_len(const fl_receiver_t *r);

#endif /* FL_RECEIVER_H */
