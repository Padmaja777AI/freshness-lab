/* Freshness Lab — HOST CSV/config loaders and result writers. SPDX-License-Identifier: MIT */
#include "sim.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define LINE_MAX_LEN 512

static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        s++;
    }
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) {
        *--e = '\0';
    }
    return s;
}

/* Splits a line on commas in place; returns field count (<= max). */
static int split_csv(char *line, char **fields, int max)
{
    int n = 0;
    char *p = line;
    while (n < max) {
        char *c = strchr(p, ',');
        fields[n++] = trim(p);
        if (c == 0) {
            break;
        }
        *c = '\0';
        p = c + 1;
    }
    return n;
}

static int parse_u32(const char *s, uint32_t *out)
{
    char *end = 0;
    unsigned long v;
    if (s == 0 || *s == '\0' || *s == '-') {
        return -1;
    }
    errno = 0;
    v = strtoul(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v > 0xFFFFFFFFul) {
        return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

static int parse_i32(const char *s, int32_t *out)
{
    char *end = 0;
    long v;
    if (s == 0 || *s == '\0') {
        return -1;
    }
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v > 2147483647L || v < -2147483647L - 1L) {
        return -1;
    }
    *out = (int32_t)v;
    return 0;
}

void workload_free(workload_t *wl)
{
    free(wl->items);
    wl->items = 0;
    wl->n = 0;
    wl->n_events = 0;
}

void trace_free(trace_t *tr)
{
    free(tr->data_lost);
    free(tr->data_delay);
    free(tr->ack_lost);
    free(tr->ack_delay);
    memset(tr, 0, sizeof(*tr));
}

int load_workload(const char *path, workload_t *wl)
{
    FILE *fp = fopen(path, "r");
    char line[LINE_MAX_LEN];
    uint32_t cap = 256;
    uint32_t lineno = 0;
    fl_time_t last_t = 0;
    if (fp == 0) {
        fprintf(stderr, "workload: cannot open %s\n", path);
        return -1;
    }
    memset(wl, 0, sizeof(*wl));
    wl->items = (wl_action_t *)malloc(cap * sizeof(wl_action_t));
    if (wl->items == 0) {
        fclose(fp);
        return -1;
    }
    while (fgets(line, sizeof(line), fp) != 0) {
        char *f[6];
        int nf;
        wl_action_t a;
        uint32_t u;
        char *l = trim(line);
        lineno++;
        if (*l == '\0' || *l == '#' || strncmp(l, "time_ms", 7) == 0) {
            continue;
        }
        nf = split_csv(l, f, 6);
        memset(&a, 0, sizeof(a));
        if (nf < 3 || parse_u32(f[0], &a.time_ms) != 0 || a.time_ms > FL_TIME_HORIZON_MS) {
            fprintf(stderr, "workload:%u: bad row\n", lineno);
            goto fail;
        }
        if (a.time_ms < last_t) {
            fprintf(stderr, "workload:%u: not sorted by time\n", lineno);
            goto fail;
        }
        last_t = a.time_ms;
        if (strcmp(f[1], "STATE") == 0) {
            if (nf < 4 || parse_u32(f[2], &u) != 0 || u >= FL_MAX_STREAMS || parse_i32(f[3], &a.value) != 0) {
                fprintf(stderr, "workload:%u: bad STATE row\n", lineno);
                goto fail;
            }
            a.action = WL_STATE;
            a.stream = (uint8_t)u;
        } else if (strcmp(f[1], "RAISE") == 0 || strcmp(f[1], "CLEAR") == 0) {
            if (nf < 5 || parse_u32(f[2], &u) != 0 || u > 255u || parse_u32(f[3], &a.deadline_rel) != 0 ||
                parse_u32(f[4], &a.retention_rel) != 0) {
                fprintf(stderr, "workload:%u: bad event row\n", lineno);
                goto fail;
            }
            a.action = (f[1][0] == 'R') ? (uint8_t)WL_RAISE : (uint8_t)WL_CLEAR;
            a.code = (uint8_t)u;
            wl->n_events++;
        } else {
            fprintf(stderr, "workload:%u: unknown action %s\n", lineno, f[1]);
            goto fail;
        }
        if (wl->n >= cap) {
            wl_action_t *p;
            cap *= 2u;
            p = (wl_action_t *)realloc(wl->items, cap * sizeof(wl_action_t));
            if (p == 0) {
                goto fail;
            }
            wl->items = p;
        }
        wl->items[wl->n++] = a;
    }
    fclose(fp);
    return 0;
fail:
    fclose(fp);
    workload_free(wl);
    return -1;
}

int load_trace(const char *path, trace_t *tr)
{
    FILE *fp = fopen(path, "r");
    char line[LINE_MAX_LEN];
    uint32_t cap = 1024;
    uint32_t lineno = 0;
    if (fp == 0) {
        fprintf(stderr, "trace: cannot open %s\n", path);
        return -1;
    }
    memset(tr, 0, sizeof(*tr));
    tr->data_lost = (uint8_t *)malloc(cap);
    tr->ack_lost = (uint8_t *)malloc(cap);
    tr->data_delay = (fl_time_t *)malloc(cap * sizeof(fl_time_t));
    tr->ack_delay = (fl_time_t *)malloc(cap * sizeof(fl_time_t));
    if (tr->data_lost == 0 || tr->ack_lost == 0 || tr->data_delay == 0 || tr->ack_delay == 0) {
        goto fail;
    }
    while (fgets(line, sizeof(line), fp) != 0) {
        char *f[6];
        int nf;
        uint32_t slot, dl, dd, al, ad;
        char *l = trim(line);
        lineno++;
        if (*l == '\0' || *l == '#' || strncmp(l, "slot", 4) == 0) {
            continue;
        }
        nf = split_csv(l, f, 6);
        if (nf < 5 || parse_u32(f[0], &slot) != 0 || parse_u32(f[1], &dl) != 0 || parse_u32(f[2], &dd) != 0 ||
            parse_u32(f[3], &al) != 0 || parse_u32(f[4], &ad) != 0 || dl > 1u || al > 1u || dd == 0 || ad == 0 ||
            dd > FL_MAX_REL_MS || ad > FL_MAX_REL_MS) {
            fprintf(stderr, "trace:%u: bad row\n", lineno);
            goto fail;
        }
        if (slot != tr->n_slots) {
            fprintf(stderr, "trace:%u: slot %u out of order (expected %u)\n", lineno, slot, tr->n_slots);
            goto fail;
        }
        if (tr->n_slots >= cap) {
            uint8_t *p1;
            uint8_t *p2;
            fl_time_t *p3;
            fl_time_t *p4;
            cap *= 2u;
            p1 = (uint8_t *)realloc(tr->data_lost, cap);
            p2 = (uint8_t *)realloc(tr->ack_lost, cap);
            p3 = (fl_time_t *)realloc(tr->data_delay, cap * sizeof(fl_time_t));
            p4 = (fl_time_t *)realloc(tr->ack_delay, cap * sizeof(fl_time_t));
            if (p1 != 0) tr->data_lost = p1;
            if (p2 != 0) tr->ack_lost = p2;
            if (p3 != 0) tr->data_delay = p3;
            if (p4 != 0) tr->ack_delay = p4;
            if (p1 == 0 || p2 == 0 || p3 == 0 || p4 == 0) {
                goto fail;
            }
        }
        tr->data_lost[tr->n_slots] = (uint8_t)dl;
        tr->data_delay[tr->n_slots] = dd;
        tr->ack_lost[tr->n_slots] = (uint8_t)al;
        tr->ack_delay[tr->n_slots] = ad;
        tr->n_slots++;
    }
    fclose(fp);
    return 0;
fail:
    fclose(fp);
    trace_free(tr);
    return -1;
}

void sim_config_defaults(sim_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->run_ms = 30000;
    cfg->slot_ms = 10;
    cfg->n_streams = 4;
    cfg->session_id = 7;
    cfg->ack_timeout_ms = 300;
    cfg->max_attempts = 8;
    cfg->aoi_threshold_ms = 2000;
    cfg->policy.family = FL_POL_EDF_RR;
    cfg->policy.event_service_ms = 40;
    cfg->policy.slack_guard_ms = 100;
    cfg->policy.state_stale_ms = 1000;
    cfg->policy.state_starvation_ms = 2000;
    strcpy(cfg->policy_name, "edf_rr");
}

int load_config(const char *path, sim_config_t *cfg)
{
    FILE *fp = fopen(path, "r");
    char line[LINE_MAX_LEN];
    uint32_t lineno = 0;
    if (fp == 0) {
        fprintf(stderr, "config: cannot open %s\n", path);
        return -1;
    }
    while (fgets(line, sizeof(line), fp) != 0) {
        char *l = trim(line);
        char *eq;
        char *key;
        char *val;
        uint32_t v;
        lineno++;
        if (*l == '\0' || *l == '#') {
            continue;
        }
        eq = strchr(l, '=');
        if (eq == 0) {
            fprintf(stderr, "config:%u: expected key=value\n", lineno);
            fclose(fp);
            return -1;
        }
        *eq = '\0';
        key = trim(l);
        val = trim(eq + 1);
        if (parse_u32(val, &v) != 0) {
            fprintf(stderr, "config:%u: bad value for %s\n", lineno, key);
            fclose(fp);
            return -1;
        }
        /* Range-check BEFORE narrowing so a wrapped value can never pass the core validator. */
        if (strcmp(key, "run_ms") == 0) {
            if (v < 1u || v > FL_TIME_HORIZON_MS) goto range;
            cfg->run_ms = v;
        } else if (strcmp(key, "slot_ms") == 0) {
            if (v < 1u || v > FL_MAX_REL_MS) goto range;
            cfg->slot_ms = v;
        } else if (strcmp(key, "n_streams") == 0) {
            if (v < 1u || v > FL_MAX_STREAMS) goto range;
            cfg->n_streams = (uint8_t)v;
        } else if (strcmp(key, "session_id") == 0) {
            if (v > 0xFFFFu) goto range;
            cfg->session_id = (uint16_t)v;
        } else if (strcmp(key, "ack_timeout_ms") == 0) {
            if (v < 1u || v > FL_MAX_REL_MS) goto range;
            cfg->ack_timeout_ms = v;
        } else if (strcmp(key, "max_attempts") == 0) {
            if (v < 1u || v > 255u) goto range;
            cfg->max_attempts = (uint8_t)v;
        } else if (strcmp(key, "aoi_threshold_ms") == 0) {
            if (v > FL_MAX_REL_MS) goto range;
            cfg->aoi_threshold_ms = v;
        } else if (strcmp(key, "event_service_ms") == 0) {
            if (v > FL_MAX_REL_MS) goto range;
            cfg->policy.event_service_ms = v;
        } else if (strcmp(key, "slack_guard_ms") == 0) {
            if (v > FL_MAX_REL_MS) goto range;
            cfg->policy.slack_guard_ms = v;
        } else if (strcmp(key, "state_stale_ms") == 0) {
            if (v > FL_MAX_REL_MS) goto range;
            cfg->policy.state_stale_ms = v;
        } else if (strcmp(key, "state_starvation_ms") == 0) {
            if (v > FL_MAX_REL_MS) goto range;
            cfg->policy.state_starvation_ms = v;
        } else {
            fprintf(stderr, "config:%u: unknown key %s\n", lineno, key);
            fclose(fp);
            return -1;
        }
        continue;
    range:
        fprintf(stderr, "config:%u: %s=%u out of range\n", lineno, key, v);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

/* ---- writers ------------------------------------------------------------ */

static const char *kind_name(uint8_t k)
{
    return k == FL_DK_STATE ? "STATE" : (k == FL_DK_EVENT ? "EVENT" : "NONE");
}

static const char *rx_outcome_name(uint8_t o)
{
    switch (o) {
    case FL_RX_REJECTED: return "REJECTED";
    case FL_RX_STATE_APPLIED: return "STATE_APPLIED";
    case FL_RX_STATE_STALE: return "STATE_STALE";
    case FL_RX_EVENT_DELIVERED: return "EVENT_DELIVERED";
    case FL_RX_EVENT_DUPLICATE: return "EVENT_DUPLICATE";
    case FL_RX_EVENT_OUT_OF_WINDOW: return "EVENT_OUT_OF_WINDOW";
    default: return "NONE";
    }
}

static const char *ack_type_name(uint8_t t)
{
    return t == FL_FT_ACK_STATE ? "ACK_STATE" : (t == FL_FT_ACK_EVENT ? "ACK_EVENT" : "");
}

int sim_write_config(FILE *fp, const sim_config_t *cfg)
{
    fprintf(fp, "# HOST SIMULATION resolved configuration\n");
    fprintf(fp, "policy=%s\nfamily=%s\ndefer=%u\nlate_demote=%u\nstarvation_override=%u\n", cfg->policy_name,
            fl_policy_family_name(cfg->policy.family), cfg->policy.defer, cfg->policy.late_demote,
            cfg->policy.starvation_override);
    fprintf(fp, "run_ms=%u\nslot_ms=%u\nn_streams=%u\nsession_id=%u\nack_timeout_ms=%u\nmax_attempts=%u\n",
            cfg->run_ms, cfg->slot_ms, cfg->n_streams, cfg->session_id, cfg->ack_timeout_ms, cfg->max_attempts);
    fprintf(fp, "aoi_threshold_ms=%u\nevent_service_ms=%u\nslack_guard_ms=%u\nstate_stale_ms=%u\nstate_starvation_ms=%u\n",
            cfg->aoi_threshold_ms, cfg->policy.event_service_ms, cfg->policy.slack_guard_ms,
            cfg->policy.state_stale_ms, cfg->policy.state_starvation_ms);
    fprintf(fp, "FL_MAX_STREAMS=%u\nFL_EVENT_CAPACITY=%u\nFL_RX_DEDUP_WINDOW=%u\nFL_RX_ACK_QUEUE=%u\n",
            (unsigned)FL_MAX_STREAMS, (unsigned)FL_EVENT_CAPACITY, (unsigned)FL_RX_DEDUP_WINDOW,
            (unsigned)FL_RX_ACK_QUEUE);
    fprintf(fp, "sizeof_sender=%u\nsizeof_receiver=%u\n", (unsigned)sizeof(fl_sender_t),
            (unsigned)sizeof(fl_receiver_t));
    return ferror(fp) ? -1 : 0;
}

int sim_write_summary(FILE *fp, const sim_result_t *r, const sim_config_t *cfg, int header)
{
    double recall = r->n_events ? (double)r->r.ev_delivered / (double)r->n_events : 0.0;
    double on_time = r->n_events ? (double)r->r.ev_on_time / (double)r->n_events : 0.0;
    double lat_mean = r->lat_n ? (double)r->lat_sum / (double)r->lat_n : 0.0;
    double conf_mean = r->conf_n ? (double)r->conf_sum / (double)r->conf_n : 0.0;
    double aoi_sum = 0.0;
    uint64_t unknown = 0, over = 0;
    fl_time_t peak = 0;
    uint32_t i;
    for (i = 0; i < cfg->n_streams; i++) {
        aoi_sum += aoi_mean(&r->aoi[i], cfg->run_ms);
        unknown += r->aoi[i].unknown_ms;
        over += r->aoi[i].over_ms;
        if (r->aoi[i].peak > peak) {
            peak = r->aoi[i].peak;
        }
    }
    if (header) {
        fprintf(fp,
                "policy,family,defer,late_demote,starvation_override,run_ms,slot_ms,n_streams,ack_timeout_ms,"
                "max_attempts,event_service_ms,slack_guard_ms,state_stale_ms,state_starvation_ms,aoi_threshold_ms,"
                "ev_generated,ev_admitted,ev_rejected_full,ev_acked,ev_retry_exhausted,ev_retention_expired,"
                "ev_pending_end,rx_ev_delivered,rx_ev_on_time,rx_ev_late,rx_ev_duplicate,rx_ev_out_of_window,"
                "rx_frames_rejected,rx_session_mismatch,rx_state_applied,rx_state_stale_dropped,"
                "rx_ack_event_dropped,rx_ack_state_coalesced,acks_ok,acks_unmatched,acks_impossible,"
                "recall,on_time_rate,delivered_but_unacked,lat_mean_ms,lat_max_ms,conf_mean_ms,conf_max_ms,"
                "aoi_mean_ms,aoi_peak_ms,unknown_ms_sum,over_threshold_ms_sum,"
                "data_frames,ack_frames,data_lost,ack_lost,bytes_data_tx,bytes_ack_tx,bytes_total_tx,"
                "event_tx,event_retries,state_tx,state_published,state_superseded,state_ack_timeouts,"
                "in_transit_end,interval_violations,ledger_mismatch,sizeof_sender,sizeof_receiver,core_error\n");
    }
    fprintf(fp, "%s,%s,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,", cfg->policy_name,
            fl_policy_family_name(cfg->policy.family), cfg->policy.defer, cfg->policy.late_demote,
            cfg->policy.starvation_override, cfg->run_ms, cfg->slot_ms, cfg->n_streams, cfg->ack_timeout_ms,
            cfg->max_attempts, cfg->policy.event_service_ms, cfg->policy.slack_guard_ms, cfg->policy.state_stale_ms,
            cfg->policy.state_starvation_ms, cfg->aoi_threshold_ms);
    fprintf(fp, "%u,%u,%u,%u,%u,%u,%u,", r->s.events_generated, r->s.events_admitted, r->s.events_rejected_full,
            r->s.events_acked, r->s.events_retry_exhausted, r->s.events_retention_expired, r->ev_pending_end);
    fprintf(fp, "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,", r->r.ev_delivered, r->r.ev_on_time, r->r.ev_late,
            r->r.ev_duplicate, r->r.ev_out_of_window, r->r.frames_rejected, r->r.session_mismatch,
            r->r.state_applied, r->r.state_stale_dropped, r->r.ack_event_dropped, r->r.ack_state_coalesced);
    fprintf(fp, "%u,%u,%u,", r->s.acks_ok, r->s.acks_unmatched, r->s.acks_impossible);
    fprintf(fp, "%.6f,%.6f,%u,%.3f,%u,%.3f,%u,", recall, on_time, r->delivered_but_unacked, lat_mean, r->lat_max,
            conf_mean, r->conf_max);
    fprintf(fp, "%.3f,%u,%llu,%llu,", cfg->n_streams ? aoi_sum / (double)cfg->n_streams : 0.0, peak,
            (unsigned long long)unknown, (unsigned long long)over);
    fprintf(fp, "%u,%u,%u,%u,%llu,%llu,%llu,", r->data_frames, r->ack_frames, r->data_lost, r->ack_lost,
            (unsigned long long)r->s.bytes_data_tx, (unsigned long long)r->r.bytes_ack_tx,
            (unsigned long long)(r->s.bytes_data_tx + r->r.bytes_ack_tx));
    fprintf(fp, "%u,%u,%u,%u,%u,%u,", r->s.event_tx, r->s.event_tx - r->s.event_first_tx, r->s.state_tx,
            r->s.state_published, r->s.state_superseded, r->s.state_ack_timeouts);
    fprintf(fp, "%u,%u,%u,%u,%u,%d\n", r->in_transit_end, r->interval_violations, r->ledger_mismatch,
            (unsigned)sizeof(fl_sender_t), (unsigned)sizeof(fl_receiver_t), r->core_error);
    return ferror(fp) ? -1 : 0;
}

int sim_write_events(FILE *fp, const sim_result_t *r)
{
    uint32_t i;
    fprintf(fp, "id,kind,code,gen_time,deadline_abs,retention_abs,admitted,outcome,terminal_time,attempts,"
                "first_tx_time,rx_first_time,rx_on_time,rx_latency_ms,rx_dups,rx_oow\n");
    for (i = 0; i < r->n_events; i++) {
        const ev_record_t *e = &r->events[i];
        fprintf(fp, "%u,%s,%u,%u,%u,%u,%u,%s,", e->id, e->kind == FL_EV_RAISE ? "RAISE" : "CLEAR", e->code,
                e->gen_time, e->deadline_abs, e->retention_abs, e->admitted, fl_terminal_name(e->outcome));
        if (e->terminal_time == FL_TIME_NONE) {
            fprintf(fp, "-,");
        } else {
            fprintf(fp, "%u,", e->terminal_time);
        }
        fprintf(fp, "%u,", e->attempts);
        if (e->first_tx_time == FL_TIME_NONE) {
            fprintf(fp, "-,");
        } else {
            fprintf(fp, "%u,", e->first_tx_time);
        }
        if (e->rx_first_time == FL_TIME_NONE) {
            fprintf(fp, "-,-,-,");
        } else {
            fprintf(fp, "%u,%u,%u,", e->rx_first_time, e->rx_on_time, e->rx_first_time - e->gen_time);
        }
        fprintf(fp, "%u,%u\n", e->rx_dups, e->rx_oow);
    }
    return ferror(fp) ? -1 : 0;
}

int sim_write_decisions(FILE *fp, const sim_result_t *r)
{
    uint32_t i;
    fprintf(fp, "slot,t,kind,reason,ref,attempt,seq,n_elig_ev,n_elig_st,lost,delay_ms,ack_type,ack_lost\n");
    for (i = 0; i < r->n_decisions; i++) {
        const dec_record_t *d = &r->decisions[i];
        uint32_t ref = d->choice.kind == FL_DK_STATE ? d->choice.stream
                       : (d->choice.kind == FL_DK_EVENT ? d->choice.event_id : 0u);
        fprintf(fp, "%u,%u,%s,%s,%u,%u,%u,%u,%u,%u,%u,%s,%u\n", d->slot, d->t, kind_name(d->choice.kind),
                fl_reason_name(d->choice.reason), ref, d->attempt, d->seq, d->choice.n_elig_ev, d->choice.n_elig_st,
                d->lost, d->delay, ack_type_name(d->ack_type), d->ack_lost);
    }
    return ferror(fp) ? -1 : 0;
}

int sim_write_state(FILE *fp, const sim_result_t *r, const sim_config_t *cfg)
{
    uint32_t i;
    fprintf(fp, "stream,published,sent,applied,stale_dropped,unknown_ms,aoi_mean_ms,aoi_area_ms2,aoi_peak_ms,"
                "final_age_ms,over_threshold_ms\n");
    for (i = 0; i < cfg->n_streams; i++) {
        const aoi_acc_t *a = &r->aoi[i];
        fprintf(fp, "%u,%u,%u,%u,%u,%u,%.3f,%.1f,%u,%u,%llu\n", i, r->sc[i].published, r->sc[i].sent, r->sc[i].applied,
                r->sc[i].stale_dropped, a->unknown_ms, aoi_mean(a, cfg->run_ms), aoi_area(a), a->peak, a->final_age,
                (unsigned long long)a->over_ms);
    }
    return ferror(fp) ? -1 : 0;
}

int sim_write_rxlog(FILE *fp, const sim_result_t *r)
{
    uint32_t i;
    fprintf(fp, "t,dir,outcome,err,stream,seq,id,gen_time,on_time\n");
    for (i = 0; i < r->n_rx; i++) {
        const rx_record_t *x = &r->rxlog[i];
        if (x->dir == 1) {
            fprintf(fp, "%u,DATA,%s,%s,%u,%u,%u,%u,%u\n", x->t, rx_outcome_name(x->outcome), fl_err_name(x->err),
                    x->stream, x->seq, x->id, x->gen_time, x->on_time);
        } else {
            fprintf(fp, "%u,ACK,%s,%s,%u,%u,%u,%u,%u\n", x->t, ack_type_name(x->outcome), fl_err_name(x->err),
                    x->stream, x->seq, x->id, 0u, 0u);
        }
    }
    return ferror(fp) ? -1 : 0;
}
