/* Freshness Lab — HOST SIMULATION loop (docs/DESIGN.md §8.1). SPDX-License-Identifier: MIT */
#include "sim.h"
#include <stdlib.h>
#include <string.h>

#define NIL 0xFFFFFFFFu

typedef struct {
    fl_time_t send_time;
    uint32_t next;
    uint8_t len;
    uint8_t bytes[FL_MAX_FRAME_LEN];
} node_t;

typedef struct {
    node_t *pool;
    uint32_t cap;
    uint32_t used;
    uint32_t *head; /* per arrival millisecond */
    uint32_t *tail;
    fl_time_t run_ms;
} transit_t;

static int transit_init(transit_t *q, uint32_t cap, fl_time_t run_ms)
{
    uint32_t i;
    q->pool = (node_t *)calloc(cap, sizeof(node_t));
    q->head = (uint32_t *)malloc((size_t)run_ms * sizeof(uint32_t));
    q->tail = (uint32_t *)malloc((size_t)run_ms * sizeof(uint32_t));
    if (q->pool == 0 || q->head == 0 || q->tail == 0) {
        return -1;
    }
    for (i = 0; i < run_ms; i++) {
        q->head[i] = NIL;
        q->tail[i] = NIL;
    }
    q->cap = cap;
    q->used = 0;
    q->run_ms = run_ms;
    return 0;
}

static void transit_free(transit_t *q)
{
    free(q->pool);
    free(q->head);
    free(q->tail);
    q->pool = 0;
    q->head = 0;
    q->tail = 0;
}

/* Returns 0 queued, 1 arrival beyond run end (counted by caller), -1 pool full. */
static int transit_push(transit_t *q, fl_time_t send_time, fl_time_t arrival, const uint8_t *bytes, uint8_t len)
{
    node_t *n;
    uint32_t idx;
    if (arrival >= q->run_ms) {
        return 1;
    }
    if (q->used >= q->cap) {
        return -1;
    }
    idx = q->used++;
    n = &q->pool[idx];
    n->send_time = send_time;
    n->next = NIL;
    n->len = len;
    memcpy(n->bytes, bytes, len);
    if (q->head[arrival] == NIL) {
        q->head[arrival] = idx;
    } else {
        q->pool[q->tail[arrival]].next = idx;
    }
    q->tail[arrival] = idx;
    return 0;
}

static void set_error(sim_result_t *out, int rc, const char *where)
{
    if (out->core_error == 0 && rc != FL_OK) {
        out->core_error = rc;
        strncpy(out->core_error_where, where, sizeof(out->core_error_where) - 1u);
        out->core_error_where[sizeof(out->core_error_where) - 1u] = '\0';
    }
}

static int rx_push(sim_result_t *out, const rx_record_t *rec)
{
    if (out->n_rx >= out->rx_cap) {
        uint32_t ncap = out->rx_cap == 0 ? 1024u : out->rx_cap * 2u;
        rx_record_t *p = (rx_record_t *)realloc(out->rxlog, (size_t)ncap * sizeof(rx_record_t));
        if (p == 0) {
            return -1;
        }
        out->rxlog = p;
        out->rx_cap = ncap;
    }
    out->rxlog[out->n_rx++] = *rec;
    return 0;
}

void sim_result_free(sim_result_t *r)
{
    free(r->events);
    free(r->decisions);
    free(r->rxlog);
    r->events = 0;
    r->decisions = 0;
    r->rxlog = 0;
}

static void note_terminal(sim_result_t *out, const fl_terminal_note_t *n)
{
    if (n->id >= 1u && n->id <= out->n_events) {
        ev_record_t *e = &out->events[n->id - 1u];
        e->outcome = n->outcome;
        e->terminal_time = n->terminal_time;
        e->attempts = n->attempts;
    }
}

int sim_run(const sim_config_t *cfg, const workload_t *wl, const trace_t *tr, sim_result_t *out)
{
    fl_sender_t *snd;
    fl_receiver_t *rcv;
    fl_sender_config_t scfg;
    transit_t tq_data;
    transit_t tq_ack;
    uint32_t n_slots_needed;
    uint32_t wi = 0;
    uint32_t i;
    fl_time_t t;
    int rc;
    int result = 0;

    if (cfg == 0 || wl == 0 || tr == 0 || out == 0) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (cfg->run_ms == 0 || cfg->slot_ms == 0 || cfg->run_ms > FL_TIME_HORIZON_MS) {
        fprintf(stderr, "sim: run_ms/slot_ms invalid\n");
        return -1;
    }
    n_slots_needed = (uint32_t)((cfg->run_ms + cfg->slot_ms - 1u) / cfg->slot_ms);
    if (tr->n_slots < n_slots_needed) {
        fprintf(stderr, "sim: trace has %u slots, run needs %u\n", tr->n_slots, n_slots_needed);
        return -1;
    }
    for (i = 0; i < n_slots_needed; i++) {
        if (tr->data_delay[i] == 0 || tr->ack_delay[i] == 0) {
            fprintf(stderr, "sim: delay must be >= 1 ms (slot %u)\n", i);
            return -1;
        }
    }
    /* Every workload row must lie inside the run: a row at t >= run_ms would
       never be generated and must not appear as a phantom ledger record. */
    for (i = 0; i < wl->n; i++) {
        if (wl->items[i].time_ms >= cfg->run_ms) {
            fprintf(stderr, "sim: workload row %u at t=%u is outside run_ms=%u (rejected)\n", i,
                    wl->items[i].time_ms, cfg->run_ms);
            return -1;
        }
    }

    snd = (fl_sender_t *)calloc(1, sizeof(fl_sender_t));
    rcv = (fl_receiver_t *)calloc(1, sizeof(fl_receiver_t));
    out->events = (ev_record_t *)calloc(wl->n_events == 0 ? 1u : wl->n_events, sizeof(ev_record_t));
    out->decisions = (dec_record_t *)calloc(n_slots_needed, sizeof(dec_record_t));
    if (snd == 0 || rcv == 0 || out->events == 0 || out->decisions == 0 ||
        transit_init(&tq_data, n_slots_needed + 1u, cfg->run_ms) != 0 ||
        transit_init(&tq_ack, n_slots_needed + 1u, cfg->run_ms) != 0) {
        fprintf(stderr, "sim: out of memory\n");
        free(snd);
        free(rcv);
        return -1;
    }
    out->ledger_cap = wl->n_events;
    out->n_events = 0; /* grows with events actually generated (IDs 1..n) */
    for (i = 0; i < wl->n_events; i++) {
        out->events[i].terminal_time = FL_TIME_NONE;
        out->events[i].rx_first_time = FL_TIME_NONE;
        out->events[i].first_tx_time = FL_TIME_NONE;
    }
    for (i = 0; i < FL_MAX_STREAMS; i++) {
        aoi_init(&out->aoi[i], cfg->aoi_threshold_ms);
    }

    memset(&scfg, 0, sizeof(scfg));
    scfg.session_id = cfg->session_id;
    scfg.n_streams = cfg->n_streams;
    scfg.policy = cfg->policy;
    scfg.ack_timeout_ms = cfg->ack_timeout_ms;
    scfg.max_attempts = cfg->max_attempts;
    rc = fl_sender_init(snd, &scfg);
    if (rc != FL_OK) {
        fprintf(stderr, "sim: fl_sender_init: %s\n", fl_err_name(rc));
        result = -1;
        goto done;
    }
    rc = fl_receiver_init(rcv, cfg->session_id, cfg->n_streams);
    if (rc != FL_OK) {
        fprintf(stderr, "sim: fl_receiver_init: %s\n", fl_err_name(rc));
        result = -1;
        goto done;
    }

    for (t = 0; t < cfg->run_ms; t++) {
        /* 1. workload actions at t, in file order */
        while (wi < wl->n && wl->items[wi].time_ms <= t) {
            const wl_action_t *a = &wl->items[wi];
            if (a->time_ms < t) {
                fprintf(stderr, "sim: workload not sorted at row %u\n", wi);
                result = -1;
                goto done;
            }
            if (a->action == WL_STATE) {
                uint8_t payload[FL_STATE_PAYLOAD_LEN];
                uint32_t v = (uint32_t)a->value;
                memset(payload, 0, sizeof(payload));
                payload[0] = (uint8_t)(v & 0xFFu);
                payload[1] = (uint8_t)((v >> 8) & 0xFFu);
                payload[2] = (uint8_t)((v >> 16) & 0xFFu);
                payload[3] = (uint8_t)((v >> 24) & 0xFFu);
                rc = fl_sender_publish_state(snd, t, a->stream, payload, 0);
                if (rc != FL_OK) {
                    set_error(out, rc, "publish_state");
                } else if (a->stream < FL_MAX_STREAMS) {
                    out->sc[a->stream].published++;
                }
            } else {
                uint8_t payload[FL_EVENT_PAYLOAD_LEN];
                uint32_t id = 0;
                uint8_t kind = (a->action == WL_RAISE) ? (uint8_t)FL_EV_RAISE : (uint8_t)FL_EV_CLEAR;
                memset(payload, 0, sizeof(payload));
                payload[0] = a->code;
                rc = fl_sender_post_event(snd, t, kind, a->code, a->deadline_rel, a->retention_rel, payload, &id);
                if (rc == FL_OK || rc == FL_ERR_EVENT_FULL) {
                    if (id >= 1u && id <= out->ledger_cap && id == out->n_events + 1u) {
                        ev_record_t *e = &out->events[id - 1u];
                        out->n_events = id;
                        e->id = id;
                        e->kind = kind;
                        e->code = a->code;
                        e->gen_time = t;
                        e->deadline_abs = t + a->deadline_rel;
                        e->retention_abs = t + a->retention_rel;
                        e->admitted = (rc == FL_OK) ? 1u : 0u;
                        if (rc == FL_ERR_EVENT_FULL) {
                            e->outcome = FL_TERM_REJECTED_FULL;
                            e->terminal_time = t;
                        }
                    } else {
                        set_error(out, FL_ERR_ARG, "event id beyond ledger");
                    }
                } else {
                    set_error(out, rc, "post_event");
                }
            }
            wi++;
        }

        /* 2. DATA arrivals at t -> receiver (enqueues ACKs) */
        for (i = tq_data.head[t]; i != NIL; i = tq_data.pool[i].next) {
            const node_t *n = &tq_data.pool[i];
            fl_rx_result_t res;
            rx_record_t rec;
            rc = fl_receiver_ingest(rcv, t, n->bytes, n->len, &res);
            memset(&rec, 0, sizeof(rec));
            rec.t = t;
            rec.dir = 1;
            rec.outcome = res.outcome;
            rec.err = res.err;
            rec.stream = res.stream;
            rec.seq = res.seq;
            rec.id = res.event_id;
            rec.gen_time = res.gen_time;
            rec.on_time = res.on_time;
            if (rx_push(out, &rec) != 0) {
                result = -1;
                goto done;
            }
            switch (res.outcome) {
            case FL_RX_STATE_APPLIED:
                aoi_apply(&out->aoi[res.stream], t, res.gen_time);
                out->sc[res.stream].applied++;
                break;
            case FL_RX_STATE_STALE:
                out->sc[res.stream].stale_dropped++;
                break;
            case FL_RX_EVENT_DELIVERED:
                if (res.event_id >= 1u && res.event_id <= out->n_events) {
                    ev_record_t *e = &out->events[res.event_id - 1u];
                    if (e->rx_first_time == FL_TIME_NONE) {
                        e->rx_first_time = t;
                        e->rx_on_time = res.on_time;
                    }
                }
                break;
            case FL_RX_EVENT_DUPLICATE:
                if (res.event_id >= 1u && res.event_id <= out->n_events) {
                    out->events[res.event_id - 1u].rx_dups++;
                }
                break;
            case FL_RX_EVENT_OUT_OF_WINDOW:
                if (res.event_id >= 1u && res.event_id <= out->n_events) {
                    out->events[res.event_id - 1u].rx_oow++;
                }
                break;
            default:
                set_error(out, rc, "receiver_ingest");
                break;
            }
        }

        /* 3. ACK arrivals at t -> sender */
        for (i = tq_ack.head[t]; i != NIL; i = tq_ack.pool[i].next) {
            const node_t *n = &tq_ack.pool[i];
            fl_terminal_note_t note;
            rx_record_t rec;
            rc = fl_sender_ingest_ack(snd, t, n->bytes, n->len, &note);
            memset(&rec, 0, sizeof(rec));
            rec.t = t;
            rec.dir = 2;
            rec.err = rc;
            rec.outcome = (uint8_t)n->bytes[1]; /* frame type */
            if (n->bytes[1] == FL_FT_ACK_EVENT) {
                rec.id = (uint32_t)n->bytes[5] | ((uint32_t)n->bytes[6] << 8) | ((uint32_t)n->bytes[7] << 16) |
                         ((uint32_t)n->bytes[8] << 24);
            } else if (n->bytes[1] == FL_FT_ACK_STATE) {
                rec.stream = n->bytes[5];
                rec.seq = (uint32_t)n->bytes[6] | ((uint32_t)n->bytes[7] << 8) | ((uint32_t)n->bytes[8] << 16) |
                          ((uint32_t)n->bytes[9] << 24);
            }
            if (rx_push(out, &rec) != 0) {
                result = -1;
                goto done;
            }
            if (rc == FL_OK && note.outcome == FL_TERM_ACKED) {
                note_terminal(out, &note);
            } else if (rc != FL_OK && rc != FL_ERR_ACK_UNMATCHED && rc != FL_ERR_ACK_IMPOSSIBLE) {
                set_error(out, rc, "ingest_ack");
            }
        }

        /* 4. transmit opportunities in both directions */
        if (t % cfg->slot_ms == 0) {
            uint32_t k = (uint32_t)(t / cfg->slot_ms);
            fl_step_result_t sr;
            dec_record_t *d = &out->decisions[k];
            uint8_t ack[FL_MAX_FRAME_LEN];
            uint8_t alen = 0;
            uint32_t j;
            rc = fl_sender_step(snd, t, &sr);
            if (rc != FL_OK) {
                set_error(out, rc, "sender_step");
            }
            for (j = 0; j < sr.n_terminal; j++) {
                note_terminal(out, &sr.terminal[j]);
            }
            d->slot = k;
            d->t = t;
            d->choice = sr.choice;
            d->attempt = sr.attempt;
            d->seq = sr.seq;
            if (sr.frame_len > 0 && sr.choice.kind == FL_DK_EVENT && sr.choice.event_id >= 1u &&
                sr.choice.event_id <= out->n_events) {
                ev_record_t *e = &out->events[sr.choice.event_id - 1u];
                e->attempts = sr.attempt;
                if (e->first_tx_time == FL_TIME_NONE) {
                    e->first_tx_time = t;
                }
                out->ledger_attempt_sum++;
                out->event_decisions++;
            }
            d->lost = 0;
            d->delay = 0;
            if (sr.frame_len > 0) {
                out->data_frames++;
                d->delay = tr->data_delay[k];
                if (tr->data_lost[k]) {
                    d->lost = 1;
                    out->data_lost++;
                } else {
                    int pr = transit_push(&tq_data, t, t + tr->data_delay[k], sr.frame, sr.frame_len);
                    if (pr == 1) {
                        out->in_transit_end++;
                    } else if (pr < 0) {
                        result = -1;
                        goto done;
                    }
                }
            }
            rc = fl_receiver_step(rcv, t, ack, (uint8_t)FL_MAX_FRAME_LEN, &alen);
            if (rc != FL_OK) {
                set_error(out, rc, "receiver_step");
            }
            d->ack_len = alen;
            d->ack_type = alen > 0 ? ack[1] : 0u;
            d->ack_lost = 0;
            if (alen > 0) {
                out->ack_frames++;
                if (tr->ack_lost[k]) {
                    d->ack_lost = 1;
                    out->ack_lost++;
                } else {
                    int pr = transit_push(&tq_ack, t, t + tr->ack_delay[k], ack, alen);
                    if (pr == 1) {
                        out->in_transit_end++;
                    } else if (pr < 0) {
                        result = -1;
                        goto done;
                    }
                }
            }
            out->n_decisions = k + 1u;
        }

        /* 5. interval check (§6.4): rx applied seq in [last_acked, max_sent] */
        for (i = 0; i < cfg->n_streams; i++) {
            const fl_receiver_stream_t *rs = fl_receiver_state(rcv, (uint8_t)i);
            uint32_t rx_seq = (rs != 0 && rs->valid) ? rs->seq : 0u;
            const fl_sender_stream_t *ss = &snd->streams[i];
            if (rx_seq < ss->last_acked_seq || rx_seq > ss->max_sent_seq) {
                out->interval_violations++;
            }
        }
    }

    /* end of run: close metrics, censor pending */
    for (i = 0; i < cfg->n_streams; i++) {
        aoi_finish(&out->aoi[i], cfg->run_ms);
    }
    for (i = 0; i < out->n_decisions; i++) {
        const dec_record_t *d = &out->decisions[i];
        if (d->choice.kind == FL_DK_STATE && d->choice.stream < FL_MAX_STREAMS) {
            out->sc[d->choice.stream].sent++;
        }
    }
    for (i = 0; i < out->n_events; i++) {
        const ev_record_t *e = &out->events[i];
        if (e->outcome == FL_TERM_PENDING) {
            out->ev_pending_end++;
        }
        if (e->rx_first_time != FL_TIME_NONE) {
            fl_time_t lat = e->rx_first_time - e->gen_time;
            out->lat_sum += lat;
            out->lat_n++;
            if (lat > out->lat_max) {
                out->lat_max = lat;
            }
            if (e->outcome != FL_TERM_ACKED) {
                out->delivered_but_unacked++;
            }
        }
        if (e->outcome == FL_TERM_ACKED) {
            fl_time_t c = e->terminal_time - e->gen_time;
            out->conf_sum += c;
            out->conf_n++;
            if (c > out->conf_max) {
                out->conf_max = c;
            }
        }
    }
    out->s = snd->stats;
    out->r = rcv->stats;
    /* Reconcile: per-event attempts (counted per transmission) must equal the
       sender's transmission counter and the number of EVENT decisions. */
    {
        uint64_t sum = 0;
        for (i = 0; i < out->n_events; i++) {
            sum += out->events[i].attempts;
        }
        out->ledger_mismatch = (sum != out->ledger_attempt_sum || sum != out->s.event_tx ||
                                out->event_decisions != out->s.event_tx || out->n_events != out->s.events_generated)
                                   ? 1u
                                   : 0u;
    }

done:
    transit_free(&tq_data);
    transit_free(&tq_ack);
    free(snd);
    free(rcv);
    return result;
}
