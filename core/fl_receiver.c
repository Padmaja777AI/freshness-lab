/* Freshness Lab — receiver core. SPDX-License-Identifier: MIT */
#include "fl_receiver.h"
#include "fl_frame.h"

static void zero_bytes(void *p, uint32_t n)
{
    uint8_t *b = (uint8_t *)p;
    uint32_t i;
    for (i = 0; i < n; i++) {
        b[i] = 0;
    }
}

static int check_time(fl_receiver_t *r, fl_time_t now)
{
    if (now > FL_TIME_HORIZON_MS) {
        return FL_ERR_TIME_HORIZON;
    }
    if (r->last_now != FL_TIME_NONE && now < r->last_now) {
        return FL_ERR_TIME_REGRESS;
    }
    r->last_now = now;
    return FL_OK;
}

int fl_receiver_init(fl_receiver_t *r, uint16_t session_id, uint8_t n_streams)
{
    if (r == 0 || n_streams < 1 || n_streams > FL_MAX_STREAMS) {
        return FL_ERR_ARG;
    }
    zero_bytes(r, (uint32_t)sizeof(*r));
    r->session_id = session_id;
    r->n_streams = n_streams;
    r->last_now = FL_TIME_NONE;
    return FL_OK;
}

uint8_t fl_receiver_ack_queue_len(const fl_receiver_t *r)
{
    return r->ack_count;
}

const fl_receiver_stream_t *fl_receiver_state(const fl_receiver_t *r, uint8_t stream)
{
    if (r == 0 || stream >= r->n_streams) {
        return 0;
    }
    return &r->streams[stream];
}

/* Returns 1 if queued, 0 if the ring was full (counted). */
static uint8_t queue_event_ack(fl_receiver_t *r, uint32_t id, uint8_t flags)
{
    uint8_t idx;
    if (r->ack_count >= FL_RX_ACK_QUEUE) {
        r->stats.ack_event_dropped++;
        return 0;
    }
    idx = (uint8_t)((r->ack_head + r->ack_count) % FL_RX_ACK_QUEUE);
    r->ack_ring[idx].id = id;
    r->ack_ring[idx].flags = flags;
    r->ack_count++;
    return 1;
}

static void queue_state_ack(fl_receiver_t *r, uint8_t stream)
{
    if (r->ack_state_pending[stream]) {
        r->stats.ack_state_coalesced++;
    }
    r->ack_state_pending[stream] = 1;
}

int fl_receiver_ingest(fl_receiver_t *r, fl_time_t now, const uint8_t *bytes, uint8_t len,
                       fl_rx_result_t *out)
{
    fl_frame_t f;
    int rc;
    uint8_t i;
    if (r == 0 || bytes == 0 || out == 0) {
        return FL_ERR_ARG;
    }
    zero_bytes(out, (uint32_t)sizeof(*out));
    rc = check_time(r, now);
    if (rc != FL_OK) {
        return rc;
    }
    rc = fl_frame_decode(bytes, len, r->session_id, &f);
    if (rc != FL_OK) {
        out->outcome = FL_RX_REJECTED;
        out->err = rc;
        if (rc == FL_ERR_FRAME_SESSION) {
            r->stats.session_mismatch++;
        } else {
            r->stats.frames_rejected++;
        }
        return rc;
    }
    if (f.type == FL_FT_STATE) {
        fl_receiver_stream_t *st;
        if (f.u.state.stream >= r->n_streams) {
            out->outcome = FL_RX_REJECTED;
            out->err = FL_ERR_FRAME_FIELD;
            r->stats.frames_rejected++;
            return FL_ERR_FRAME_FIELD;
        }
        r->stats.frames_ok++;
        st = &r->streams[f.u.state.stream];
        out->stream = f.u.state.stream;
        out->seq = f.u.state.seq;
        out->gen_time = f.u.state.gen_time;
        for (i = 0; i < (uint8_t)FL_STATE_PAYLOAD_LEN; i++) {
            out->payload[i] = f.u.state.payload[i];
        }
        if (!st->valid || f.u.state.seq > st->seq) {
            st->valid = 1;
            st->seq = f.u.state.seq;
            st->gen_time = f.u.state.gen_time;
            for (i = 0; i < (uint8_t)FL_STATE_PAYLOAD_LEN; i++) {
                st->payload[i] = f.u.state.payload[i];
            }
            out->outcome = FL_RX_STATE_APPLIED;
            r->stats.state_applied++;
        } else {
            out->outcome = FL_RX_STATE_STALE;
            r->stats.state_stale_dropped++;
        }
        queue_state_ack(r, f.u.state.stream); /* reports applied seq at ACK time */
        return FL_OK;
    }
    if (f.type == FL_FT_EVENT) {
        const uint32_t id = f.u.event.id;
        r->stats.frames_ok++;
        out->event_id = id;
        out->ev_kind = f.u.event.kind;
        out->ev_code = f.u.event.code;
        out->gen_time = f.u.event.gen_time;
        out->deadline_abs = f.u.event.deadline_abs;
        out->on_time = (now <= f.u.event.deadline_abs) ? 1u : 0u;
        for (i = 0; i < (uint8_t)FL_EVENT_PAYLOAD_LEN; i++) {
            out->payload[i] = f.u.event.payload[i];
        }
        if (id == 0) {
            out->outcome = FL_RX_REJECTED;
            out->err = FL_ERR_FRAME_FIELD;
            r->stats.frames_rejected++;
            r->stats.frames_ok--;
            return FL_ERR_FRAME_FIELD;
        }
        if (id > r->highest_id) {
            uint32_t shift = id - r->highest_id;
            if (shift >= 64u) {
                r->dedup_bits = 0;
            } else {
                r->dedup_bits <<= shift;
            }
            r->dedup_bits |= 1u;
            r->highest_id = id;
            out->outcome = FL_RX_EVENT_DELIVERED;
        } else {
            uint32_t d = r->highest_id - id;
            if (d < FL_RX_DEDUP_WINDOW) {
                uint64_t bit = (uint64_t)1u << d;
                if (r->dedup_bits & bit) {
                    out->outcome = FL_RX_EVENT_DUPLICATE;
                } else {
                    r->dedup_bits |= bit;
                    out->outcome = FL_RX_EVENT_DELIVERED;
                }
            } else {
                out->outcome = FL_RX_EVENT_OUT_OF_WINDOW;
            }
        }
        if (out->outcome == FL_RX_EVENT_DELIVERED) {
            r->stats.ev_delivered++;
            if (out->on_time) {
                r->stats.ev_on_time++;
            } else {
                r->stats.ev_late++;
            }
            out->ack_dropped = (uint8_t)(queue_event_ack(r, id, 0) ? 0u : 1u);
        } else if (out->outcome == FL_RX_EVENT_DUPLICATE) {
            r->stats.ev_duplicate++;
            out->ack_dropped = (uint8_t)(queue_event_ack(r, id, FL_ACKF_DUPLICATE) ? 0u : 1u);
        } else {
            r->stats.ev_out_of_window++; /* no ACK: cannot vouch for it */
        }
        return FL_OK;
    }
    out->outcome = FL_RX_REJECTED;
    out->err = FL_ERR_FRAME_TYPE;
    r->stats.frames_rejected++;
    return FL_ERR_FRAME_TYPE;
}

int fl_receiver_step(fl_receiver_t *r, fl_time_t now, uint8_t *ack_out, uint8_t cap, uint8_t *len_out)
{
    fl_frame_t f;
    int rc;
    uint8_t k;
    if (r == 0 || ack_out == 0 || len_out == 0) {
        return FL_ERR_ARG;
    }
    *len_out = 0;
    rc = check_time(r, now);
    if (rc != FL_OK) {
        return rc;
    }
    r->stats.steps++;
    f.session = r->session_id;
    if (r->ack_count > 0) {
        const fl_ack_entry_t *e = &r->ack_ring[r->ack_head];
        f.type = FL_FT_ACK_EVENT;
        f.u.ack_event.id = e->id;
        f.u.ack_event.flags = e->flags;
        rc = fl_frame_encode(&f, ack_out, cap, len_out);
        if (rc != FL_OK) {
            return rc;
        }
        r->ack_head = (uint8_t)((r->ack_head + 1u) % FL_RX_ACK_QUEUE);
        r->ack_count--;
        r->stats.ack_tx++;
        r->stats.bytes_ack_tx += *len_out;
        return FL_OK;
    }
    for (k = 0; k < r->n_streams; k++) {
        uint8_t sid = (uint8_t)((r->ack_rr_next + k) % r->n_streams);
        if (r->ack_state_pending[sid]) {
            const fl_receiver_stream_t *st = &r->streams[sid];
            f.type = FL_FT_ACK_STATE;
            f.u.ack_state.stream = sid;
            f.u.ack_state.applied_seq = st->valid ? st->seq : 0u;
            f.u.ack_state.applied_gen = st->valid ? st->gen_time : 0u;
            rc = fl_frame_encode(&f, ack_out, cap, len_out);
            if (rc != FL_OK) {
                return rc;
            }
            r->ack_state_pending[sid] = 0;
            r->ack_rr_next = (uint8_t)((sid + 1u) % r->n_streams);
            r->stats.ack_tx++;
            r->stats.bytes_ack_tx += *len_out;
            return FL_OK;
        }
    }
    return FL_OK;
}
