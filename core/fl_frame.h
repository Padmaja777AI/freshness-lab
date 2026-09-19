/*
 * Freshness Lab — wire format codec (docs/DESIGN.md §5).
 * Little-endian, exact per-type lengths, length checked before any field read.
 * SPDX-License-Identifier: MIT
 */
#ifndef FL_FRAME_H
#define FL_FRAME_H

#include "fl_types.h"

typedef struct {
    uint8_t type;     /* enum fl_frame_type */
    uint16_t session;
    union {
        struct {
            uint8_t stream;
            uint32_t seq;
            fl_time_t gen_time;
            uint8_t payload[FL_STATE_PAYLOAD_LEN];
        } state;
        struct {
            uint32_t id;
            uint8_t kind;
            uint8_t code;
            fl_time_t gen_time;
            fl_time_t deadline_abs;
            uint8_t payload[FL_EVENT_PAYLOAD_LEN];
        } event;
        struct {
            uint8_t stream;
            uint32_t applied_seq;
            fl_time_t applied_gen;
        } ack_state;
        struct {
            uint32_t id;
            uint8_t flags;
        } ack_event;
    } u;
} fl_frame_t;

/* Exact encoded size for a frame type, 0 if the type is unknown. */
uint8_t fl_frame_size(uint8_t type);

/* Encodes into out[0..cap). Returns FL_OK and sets *len_out, or an error. */
int fl_frame_encode(const fl_frame_t *f, uint8_t *out, uint8_t cap, uint8_t *len_out);

/*
 * Decodes bytes[0..len). Validation order: len >= header, magic, type known,
 * declared len == type size == len, session == expected. Nothing is read
 * beyond len. Returns FL_OK or a specific FL_ERR_FRAME_* code.
 */
int fl_frame_decode(const uint8_t *bytes, uint8_t len, uint16_t expected_session, fl_frame_t *out);

#endif /* FL_FRAME_H */
