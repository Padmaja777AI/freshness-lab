/* Freshness Lab — transmitter core. SPDX-License-Identifier: MIT */
#include "fl_sender.h"
#include "fl_frame.h"

static int check_time(fl_sender_t *s, fl_time_t now)
{
    if (now > FL_TIME_HORIZON_MS) {
        return FL_ERR_TIME_HORIZON;
    }
    if (s->last_now != FL_TIME_NONE && now < s->last_now) {
        return FL_ERR_TIME_REGRESS;
    }
    s->last_now = now;
    return FL_OK;
}

static void zero_bytes(void *p, uint32_t n)
{
    uint8_t *b = (uint8_t *)p;
    uint32_t i;
    for (i = 0; i < n; i++) {
        b[i] = 0;
    }
}

int fl_sender_init(fl_sender_t *s, const fl_sender_config_t *cfg)
{
    uint8_t i;
    if (s == 0 || cfg == 0) {
        return FL_ERR_ARG;
    }
    if (cfg->n_streams < 1 || cfg->n_streams > FL_MAX_STREAMS) {
        return FL_ERR_ARG;
    }
    if (cfg->ack_timeout_ms < 1 || cfg->ack_timeout_ms > FL_MAX_REL_MS) {
        return FL_ERR_ARG;
    }
    if (cfg->max_attempts < 1) {
        return FL_ERR_ARG;
    }
    if (cfg->policy.family > FL_POL_FIFO) {
        return FL_ERR_ARG;
    }
    if (cfg->policy.event_service_ms > FL_MAX_REL_MS || cfg->policy.slack_guard_ms > FL_MAX_REL_MS ||
        cfg->policy.state_stale_ms > FL_MAX_REL_MS || cfg->policy.state_starvation_ms > FL_MAX_REL_MS) {
        return FL_ERR_ARG;
    }
    zero_bytes(s, (uint32_t)sizeof(*s));
    s->cfg = *cfg;
    s->last_now = FL_TIME_NONE;
    for (i = 0; i < FL_MAX_STREAMS; i++) {
        s->streams[i].last_acked_gen = FL_TIME_NONE;
        s->streams[i].max_sent_gen = FL_TIME_NONE;
        s->streams[i].last_tx_time = FL_TIME_NONE;
        s->streams[i].next_seq = 1;
    }
    return FL_OK;
}

int fl_sender_publish_state(fl_sender_t *s, fl_time_t now, uint8_t stream,
                            const uint8_t payload[FL_STATE_PAYLOAD_LEN], uint32_t *seq_out)
{
    fl_sender_stream_t *st;
    uint8_t i;
    int rc;
    if (s == 0 || payload == 0) {
        return FL_ERR_ARG;
    }
    if (stream >= s->cfg.n_streams) {
        return FL_ERR_ARG;
    }
    rc = check_time(s, now);
    if (rc != FL_OK) {
        return rc;
    }
    st = &s->streams[stream];
    if (st->next_seq == UINT32_MAX) {
        return FL_ERR_SEQ_EXHAUSTED;
    }
    /* Supersede: a waiting (unsent) snapshot is replaced. In-flight untouched. */
    if (st->latest_valid && st->latest_seq > st->max_sent_seq) {
        s->stats.state_superseded++;
    }
    st->latest_valid = 1;
    st->latest_seq = st->next_seq++;
    st->latest_gen = now;
    for (i = 0; i < (uint8_t)FL_STATE_PAYLOAD_LEN; i++) {
        st->latest_payload[i] = payload[i];
    }
    s->stats.state_published++;
    if (seq_out != 0) {
        *seq_out = st->latest_seq;
    }
    return FL_OK;
}

int fl_sender_post_event(fl_sender_t *s, fl_time_t now, uint8_t kind, uint8_t code,
                         fl_time_t deadline_rel, fl_time_t retention_rel,
                         const uint8_t payload[FL_EVENT_PAYLOAD_LEN], uint32_t *id_out)
{
    uint8_t i;
    uint32_t id;
    fl_sender_event_t *slot = 0;
    int rc;
    if (s == 0 || payload == 0) {
        return FL_ERR_ARG;
    }
    if (kind != FL_EV_RAISE && kind != FL_EV_CLEAR) {
        return FL_ERR_ARG;
    }
    if (retention_rel > FL_MAX_REL_MS || deadline_rel > retention_rel) {
        return FL_ERR_REL_RANGE;
    }
    rc = check_time(s, now);
    if (rc != FL_OK) {
        return rc;
    }
    if (s->stats.events_generated >= UINT32_MAX - 1u) {
        return FL_ERR_ID_EXHAUSTED; /* next id would be UINT32_MAX */
    }
    id = s->stats.events_generated + 1u;
    s->stats.events_generated = id;
    if (id_out != 0) {
        *id_out = id;
    }
    for (i = 0; i < FL_EVENT_CAPACITY; i++) {
        if (!s->events[i].used) {
            slot = &s->events[i];
            break;
        }
    }
    if (slot == 0) {
        s->stats.events_rejected_full++;
        return FL_ERR_EVENT_FULL;
    }
    slot->used = 1;
    slot->id = id;
    slot->kind = kind;
    slot->code = code;
    slot->gen_time = now;
    slot->deadline_abs = now + deadline_rel;    /* cannot wrap: §3 */
    slot->retention_abs = now + retention_rel;
    slot->attempts = 0;
    slot->in_flight = 0;
    slot->sent_time = FL_TIME_NONE;
    for (i = 0; i < (uint8_t)FL_EVENT_PAYLOAD_LEN; i++) {
        slot->payload[i] = payload[i];
    }
    s->stats.events_admitted++;
    return FL_OK;
}

uint8_t fl_sender_pending_events(const fl_sender_t *s)
{
    uint8_t i;
    uint8_t n = 0;
    for (i = 0; i < FL_EVENT_CAPACITY; i++) {
        if (s->events[i].used) {
            n++;
        }
    }
    return n;
}

static void add_note(fl_step_result_t *r, uint32_t id, uint8_t outcome, fl_time_t t, uint8_t attempts)
{
    if (r->n_terminal < FL_EVENT_CAPACITY) {
        r->terminal[r->n_terminal].id = id;
        r->terminal[r->n_terminal].outcome = outcome;
        r->terminal[r->n_terminal].terminal_time = t;
        r->terminal[r->n_terminal].attempts = attempts;
        r->n_terminal++;
    }
}

/* Housekeeping (§6.3): retention expiry first, then ACK timeouts. */
static void housekeeping(fl_sender_t *s, fl_time_t now, fl_step_result_t *r)
{
    uint8_t i;
    for (i = 0; i < FL_EVENT_CAPACITY; i++) {
        fl_sender_event_t *e = &s->events[i];
        if (e->used && now >= e->retention_abs) {
            add_note(r, e->id, FL_TERM_RETENTION_EXPIRED, now, e->attempts);
            s->stats.events_retention_expired++;
            e->used = 0;
        }
    }
    for (i = 0; i < FL_EVENT_CAPACITY; i++) {
        fl_sender_event_t *e = &s->events[i];
        if (e->used && e->in_flight && (now - e->sent_time) >= s->cfg.ack_timeout_ms) {
            e->in_flight = 0;
            if (e->attempts >= s->cfg.max_attempts) {
                add_note(r, e->id, FL_TERM_RETRY_EXHAUSTED, now, e->attempts);
                s->stats.events_retry_exhausted++;
                e->used = 0;
            }
        }
    }
    for (i = 0; i < s->cfg.n_streams; i++) {
        fl_sender_stream_t *st = &s->streams[i];
        if (st->inflight_valid && (now - st->inflight_sent) >= s->cfg.ack_timeout_ms) {
            st->inflight_valid = 0;
            s->stats.state_ack_timeouts++;
            r->state_timeouts++;
        }
    }
}

void fl_sender_build_view(const fl_sender_t *s, fl_time_t now, fl_sched_view_t *v)
{
    uint8_t i;
    v->now = now;
    v->params = s->cfg.policy;
    v->n_events = 0;
    v->n_streams = 0;
    v->n_streams_total = s->cfg.n_streams;
    v->rr_next = s->rr_next;
    for (i = 0; i < FL_EVENT_CAPACITY; i++) {
        const fl_sender_event_t *e = &s->events[i];
        if (e->used && !e->in_flight && now < e->retention_abs) {
            /* insertion sort by (deadline_abs, id) */
            uint8_t j = v->n_events;
            fl_view_event_t ve;
            ve.id = e->id;
            ve.deadline_abs = e->deadline_abs;
            ve.gen_time = e->gen_time;
            while (j > 0 && (v->events[j - 1u].deadline_abs > ve.deadline_abs ||
                             (v->events[j - 1u].deadline_abs == ve.deadline_abs &&
                              v->events[j - 1u].id > ve.id))) {
                v->events[j] = v->events[j - 1u];
                j--;
            }
            v->events[j] = ve;
            v->n_events++;
        }
    }
    for (i = 0; i < s->cfg.n_streams; i++) {
        const fl_sender_stream_t *st = &s->streams[i];
        if (st->latest_valid && st->latest_seq > st->last_acked_seq && !st->inflight_valid) {
            fl_view_stream_t *vs = &v->streams[v->n_streams++];
            vs->stream = i;
            vs->latest_gen = st->latest_gen;
            vs->last_tx_time = st->last_tx_time;
            vs->last_acked_gen = st->last_acked_gen;
        }
    }
}

int fl_sender_step(fl_sender_t *s, fl_time_t now, fl_step_result_t *r)
{
    fl_sched_view_t view;
    fl_frame_t f;
    uint8_t rr_next;
    int rc;
    uint8_t i;
    if (s == 0 || r == 0) {
        return FL_ERR_ARG;
    }
    zero_bytes(r, (uint32_t)sizeof(*r));
    rc = check_time(s, now);
    if (rc != FL_OK) {
        return rc;
    }
    s->stats.steps++;
    housekeeping(s, now, r);
    fl_sender_build_view(s, now, &view);
    rc = fl_policy_select(&view, &r->choice, &rr_next);
    if (rc != FL_OK) {
        return rc;
    }
    s->rr_next = rr_next;

    if (r->choice.kind == FL_DK_EVENT) {
        fl_sender_event_t *e = 0;
        for (i = 0; i < FL_EVENT_CAPACITY; i++) {
            if (s->events[i].used && s->events[i].id == r->choice.event_id) {
                e = &s->events[i];
                break;
            }
        }
        if (e == 0 || e->in_flight || e->attempts >= s->cfg.max_attempts) {
            return FL_ERR_ARG; /* policy chose something not eligible: bug */
        }
        f.type = FL_FT_EVENT;
        f.session = s->cfg.session_id;
        f.u.event.id = e->id;
        f.u.event.kind = e->kind;
        f.u.event.code = e->code;
        f.u.event.gen_time = e->gen_time;
        f.u.event.deadline_abs = e->deadline_abs;
        for (i = 0; i < (uint8_t)FL_EVENT_PAYLOAD_LEN; i++) {
            f.u.event.payload[i] = e->payload[i];
        }
        rc = fl_frame_encode(&f, r->frame, (uint8_t)FL_MAX_FRAME_LEN, &r->frame_len);
        if (rc != FL_OK) {
            return rc;
        }
        e->attempts++;
        e->in_flight = 1;
        e->sent_time = now;
        r->attempt = e->attempts;
        s->stats.event_tx++;
        if (e->attempts == 1) {
            s->stats.event_first_tx++;
        }
        s->stats.bytes_data_tx += r->frame_len;
    } else if (r->choice.kind == FL_DK_STATE) {
        fl_sender_stream_t *st;
        if (r->choice.stream >= s->cfg.n_streams) {
            return FL_ERR_ARG;
        }
        st = &s->streams[r->choice.stream];
        if (!st->latest_valid || st->inflight_valid || st->latest_seq <= st->last_acked_seq) {
            return FL_ERR_ARG;
        }
        f.type = FL_FT_STATE;
        f.session = s->cfg.session_id;
        f.u.state.stream = r->choice.stream;
        f.u.state.seq = st->latest_seq;
        f.u.state.gen_time = st->latest_gen;
        for (i = 0; i < (uint8_t)FL_STATE_PAYLOAD_LEN; i++) {
            f.u.state.payload[i] = st->latest_payload[i];
        }
        rc = fl_frame_encode(&f, r->frame, (uint8_t)FL_MAX_FRAME_LEN, &r->frame_len);
        if (rc != FL_OK) {
            return rc;
        }
        st->inflight_valid = 1;
        st->inflight_seq = st->latest_seq;
        st->inflight_gen = st->latest_gen;
        st->inflight_sent = now;
        st->max_sent_seq = st->latest_seq; /* seq is monotone, so this is the max */
        st->max_sent_gen = st->latest_gen;
        st->last_tx_time = now;
        r->seq = st->latest_seq;
        s->stats.state_tx++;
        s->stats.bytes_data_tx += r->frame_len;
    }
    return FL_OK;
}

int fl_sender_ingest_ack(fl_sender_t *s, fl_time_t now, const uint8_t *bytes, uint8_t len,
                         fl_terminal_note_t *note)
{
    fl_frame_t f;
    int rc;
    uint8_t i;
    if (s == 0 || bytes == 0 || note == 0) {
        return FL_ERR_ARG;
    }
    note->id = 0;
    note->outcome = FL_TERM_PENDING;
    note->terminal_time = FL_TIME_NONE;
    note->attempts = 0;
    rc = check_time(s, now);
    if (rc != FL_OK) {
        return rc;
    }
    rc = fl_frame_decode(bytes, len, s->cfg.session_id, &f);
    if (rc != FL_OK) {
        if (rc == FL_ERR_FRAME_SESSION) {
            s->stats.acks_session_mismatch++;
        } else {
            s->stats.acks_rejected_frame++;
        }
        return rc;
    }
    if (f.type == FL_FT_ACK_STATE) {
        fl_sender_stream_t *st;
        const uint32_t seq = f.u.ack_state.applied_seq;
        const fl_time_t gen = f.u.ack_state.applied_gen;
        if (f.u.ack_state.stream >= s->cfg.n_streams) {
            s->stats.acks_rejected_frame++;
            return FL_ERR_FRAME_FIELD;
        }
        st = &s->streams[f.u.ack_state.stream];
        /* Impossible ACKs (§6.3): beyond anything ever sent, or from the future. */
        if (seq > st->max_sent_seq || gen > now ||
            (st->max_sent_gen != FL_TIME_NONE && gen > st->max_sent_gen) ||
            (seq == st->last_acked_seq && seq != 0 && gen != st->last_acked_gen) ||
            (st->inflight_valid && seq == st->inflight_seq && gen != st->inflight_gen)) {
            s->stats.acks_impossible++;
            return FL_ERR_ACK_IMPOSSIBLE;
        }
        if (seq > st->last_acked_seq) {
            st->last_acked_seq = seq;
            st->last_acked_gen = gen;
        }
        if (st->inflight_valid && seq >= st->inflight_seq) {
            st->inflight_valid = 0;
        }
        s->stats.acks_ok++;
        return FL_OK;
    }
    if (f.type == FL_FT_ACK_EVENT) {
        const uint32_t id = f.u.ack_event.id;
        fl_sender_event_t *e = 0;
        if (id == 0 || id > s->stats.events_generated) {
            s->stats.acks_impossible++;
            return FL_ERR_ACK_IMPOSSIBLE;
        }
        for (i = 0; i < FL_EVENT_CAPACITY; i++) {
            if (s->events[i].used && s->events[i].id == id) {
                e = &s->events[i];
                break;
            }
        }
        if (e == 0) {
            s->stats.acks_unmatched++;
            return FL_ERR_ACK_UNMATCHED;
        }
        if (e->attempts == 0) {
            s->stats.acks_impossible++; /* admitted but never transmitted */
            return FL_ERR_ACK_IMPOSSIBLE;
        }
        note->id = id;
        note->outcome = FL_TERM_ACKED;
        note->terminal_time = now;
        note->attempts = e->attempts;
        e->used = 0;
        s->stats.events_acked++;
        s->stats.acks_ok++;
        return FL_OK;
    }
    s->stats.acks_rejected_frame++;
    return FL_ERR_FRAME_TYPE;
}
