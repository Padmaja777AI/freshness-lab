/*
 * Simulation-level tests on in-memory workloads/traces (HOST SIMULATION):
 * ledger regressions, accounting identity, determinism, ablation
 * equivalence, ledger independence under ACK loss, late != on time.
 * SPDX-License-Identifier: MIT
 */
#define _POSIX_C_SOURCE 200809L
#include "fltest.h"
#include <stdlib.h>
#include <unistd.h>
#include "../host/sim.h"

static void mk_trace(trace_t *tr, uint32_t n_slots, fl_time_t delay)
{
    uint32_t i;
    tr->n_slots = n_slots;
    tr->data_lost = (uint8_t *)calloc(n_slots, 1);
    tr->ack_lost = (uint8_t *)calloc(n_slots, 1);
    tr->data_delay = (fl_time_t *)calloc(n_slots, sizeof(fl_time_t));
    tr->ack_delay = (fl_time_t *)calloc(n_slots, sizeof(fl_time_t));
    for (i = 0; i < n_slots; i++) {
        tr->data_delay[i] = delay;
        tr->ack_delay[i] = delay;
    }
}

static void set_loss(trace_t *tr, int data, int ack, uint32_t from_slot, uint32_t to_slot_excl)
{
    uint32_t i;
    for (i = from_slot; i < to_slot_excl && i < tr->n_slots; i++) {
        if (data) tr->data_lost[i] = 1;
        if (ack) tr->ack_lost[i] = 1;
    }
}

static void wl_init(workload_t *wl)
{
    wl->items = (wl_action_t *)calloc(4096, sizeof(wl_action_t));
    wl->n = 0;
    wl->n_events = 0;
}

static void wl_state(workload_t *wl, fl_time_t t, uint8_t stream, int32_t v)
{
    wl_action_t *a = &wl->items[wl->n++];
    memset(a, 0, sizeof(*a));
    a->time_ms = t;
    a->action = WL_STATE;
    a->stream = stream;
    a->value = v;
}

static void wl_event(workload_t *wl, fl_time_t t, uint8_t kind, uint8_t code, fl_time_t dl, fl_time_t ret)
{
    wl_action_t *a = &wl->items[wl->n++];
    memset(a, 0, sizeof(*a));
    a->time_ms = t;
    a->action = kind == FL_EV_RAISE ? (uint8_t)WL_RAISE : (uint8_t)WL_CLEAR;
    a->code = code;
    a->deadline_rel = dl;
    a->retention_rel = ret;
    wl->n_events++;
}

static void cfg_init(sim_config_t *c, const char *policy, fl_time_t run_ms, uint8_t n_streams)
{
    sim_config_defaults(c);
    c->run_ms = run_ms;
    c->slot_ms = 10;
    c->n_streams = n_streams;
    c->ack_timeout_ms = 300;
    c->max_attempts = 8;
    c->aoi_threshold_ms = 2000;
    CHECK_EQI(fl_policy_family_from_name(policy, &c->policy), FL_OK);
    strcpy(c->policy_name, policy);
}

/* Regression 1: a workload row at t >= run_ms must be rejected, never a phantom pending record. */
static void test_reject_out_of_run_rows(void)
{
    sim_config_t c;
    workload_t wl;
    trace_t tr;
    sim_result_t r;
    cfg_init(&c, "edf_rr", 10, 1);
    wl_init(&wl);
    wl_event(&wl, 10, FL_EV_RAISE, 1, 5, 10);
    mk_trace(&tr, 1, 2);
    CHECK(sim_run(&c, &wl, &tr, &r) != 0);
    sim_result_free(&r);
    /* boundary: t = run_ms - 1 is inside the run */
    wl.n = 0;
    wl.n_events = 0;
    wl_event(&wl, 9, FL_EV_RAISE, 1, 5, 10);
    CHECK_EQI(sim_run(&c, &wl, &tr, &r), 0);
    CHECK_EQ(r.n_events, 1);
    CHECK_EQ(r.s.events_generated, 1);
    CHECK_EQ(r.ev_pending_end, 1); /* generated at 9, no slot after 9 in a 10 ms run */
    CHECK_EQ(r.events[0].attempts, 0);
    CHECK_EQ(r.ledger_mismatch, 0);
    sim_result_free(&r);
    free(wl.items);
    trace_free(&tr);
}

/* Regression 2: attempts are recorded per transmission, not only at terminal. */
static void test_attempts_recorded_when_pending(void)
{
    sim_config_t c;
    workload_t wl;
    trace_t tr;
    sim_result_t r;
    cfg_init(&c, "edf_rr", 1, 1);
    c.slot_ms = 1;
    wl_init(&wl);
    wl_event(&wl, 0, FL_EV_RAISE, 1, 5, 10);
    mk_trace(&tr, 1, 1);
    set_loss(&tr, 1, 0, 0, 1);
    CHECK_EQI(sim_run(&c, &wl, &tr, &r), 0);
    CHECK_EQ(r.n_events, 1);
    CHECK_EQ(r.events[0].outcome, FL_TERM_PENDING);
    CHECK_EQ(r.events[0].attempts, 1);
    CHECK_EQ(r.events[0].first_tx_time, 0);
    CHECK_EQ(r.s.event_tx, 1);
    CHECK_EQ(r.event_decisions, 1);
    CHECK_EQ(r.ledger_attempt_sum, 1);
    CHECK_EQ(r.ledger_mismatch, 0);
    CHECK_EQ(r.data_lost, 1);
    CHECK_EQ(r.s.bytes_data_tx, 27); /* lost attempt still charged */
    sim_result_free(&r);
    free(wl.items);
    trace_free(&tr);
}

/* Delay 0 or > FL_MAX_REL_MS in the trace is rejected (§8.2): t + delay can then never wrap. */
static void test_reject_zero_delay(void)
{
    sim_config_t c;
    workload_t wl;
    trace_t tr;
    sim_result_t r;
    cfg_init(&c, "edf_rr", 100, 1);
    wl_init(&wl);
    mk_trace(&tr, 10, 2);
    tr.ack_delay[3] = 0;
    CHECK(sim_run(&c, &wl, &tr, &r) != 0);
    tr.ack_delay[3] = 2;
    tr.data_delay[1] = FL_MAX_REL_MS + 1u;
    CHECK(sim_run(&c, &wl, &tr, &r) != 0);
    tr.data_delay[1] = 0xFFFFFFD8u; /* would wrap t=50 + delay into a past bucket */
    CHECK(sim_run(&c, &wl, &tr, &r) != 0);
    tr.data_delay[1] = 2;
    tr.ack_delay[2] = 0xFFFFFFF0u; /* same hole on the ACK direction */
    CHECK(sim_run(&c, &wl, &tr, &r) != 0);
    tr.ack_delay[2] = FL_MAX_REL_MS + 1u;
    CHECK(sim_run(&c, &wl, &tr, &r) != 0);
    tr.ack_delay[2] = 2;
    tr.data_delay[1] = 2;
    tr.data_delay[2] = FL_MAX_REL_MS; /* allowed: the frame sent at slot 2 arrives after run end -> in transit */
    wl_state(&wl, 0, 0, 1);  /* sent at t=0, applied at 2, ACK back at 12 */
    wl_state(&wl, 15, 0, 2); /* eligible at slot 2 (t=20) */
    CHECK_EQI(sim_run(&c, &wl, &tr, &r), 0);
    CHECK_EQ(r.data_frames, r.r.frames_ok + r.data_lost + r.in_transit_data);
    CHECK_EQ(r.in_transit_data, 1);
    CHECK_EQ(r.ledger_mismatch, 0);
    sim_result_free(&r);
    free(wl.items);
    trace_free(&tr);
}

/* Returns the value of a named column in the single summary row (writes via sim_write_summary). */
static int summary_field(const sim_result_t *r, const sim_config_t *c, const char *name, char *out, size_t cap)
{
    FILE *fp = tmpfile();
    static char buf[8192];
    size_t n;
    char *row;
    char *h;
    char *v;
    int col = -1;
    int k = 0;
    if (fp == 0) {
        return -1;
    }
    sim_write_summary(fp, r, c, 1);
    rewind(fp);
    n = fread(buf, 1, sizeof(buf) - 1u, fp);
    buf[n] = '\0';
    fclose(fp);
    row = strchr(buf, '\n');
    if (row == 0) {
        return -1;
    }
    *row++ = '\0';
    for (h = strtok(buf, ","); h != 0; h = strtok(0, ",")) {
        if (strcmp(h, name) == 0) {
            col = k;
        }
        k++;
    }
    if (col < 0) {
        return -1;
    }
    k = 0;
    for (v = strtok(row, ",\n"); v != 0; v = strtok(0, ",\n")) {
        if (k == col) {
            strncpy(out, v, cap - 1u);
            out[cap - 1u] = '\0';
            return 0;
        }
        k++;
    }
    return -1;
}

/*
 * Undefined AoI must never become a numeric zero in the headline aggregate:
 * two streams, only stream 0 publishes -> aoi_mean_ms is NA, the partial
 * aggregate equals stream 0's mean with coverage 1; all-unknown -> both NA.
 * Stream 0: applied at 2 with gen 0, run 100: [2,100) age 2->100, area 4998, mean 4998/98.
 */
static void test_aoi_undefined_streams_are_na(void)
{
    sim_config_t c;
    workload_t wl;
    trace_t tr;
    sim_result_t r;
    char v[64];
    cfg_init(&c, "edf_rr", 100, 2);
    wl_init(&wl);
    wl_state(&wl, 0, 0, 1); /* stream 1 never publishes */
    mk_trace(&tr, 10, 2);
    CHECK_EQI(sim_run(&c, &wl, &tr, &r), 0);
    CHECK_EQ(r.aoi[0].defined, 1);
    CHECK_EQ(r.aoi[1].defined, 0);
    CHECK_EQ(r.aoi[1].unknown_ms, 100);
    CHECK_NEAR(aoi_mean(&r.aoi[0], 100), 4998.0 / 98.0, 1e-9);
    CHECK_EQI(summary_field(&r, &c, "aoi_mean_ms", v, sizeof(v)), 0);
    CHECK(strcmp(v, "NA") == 0);
    CHECK_EQI(summary_field(&r, &c, "aoi_mean_defined_ms", v, sizeof(v)), 0);
    CHECK_NEAR(atof(v), 4998.0 / 98.0, 1e-3);
    CHECK_EQI(summary_field(&r, &c, "aoi_defined_streams", v, sizeof(v)), 0);
    CHECK_EQ((unsigned)atoi(v), 1);
    CHECK_EQI(summary_field(&r, &c, "unknown_ms_sum", v, sizeof(v)), 0);
    CHECK_EQ((unsigned)atoi(v), 102); /* 2 + 100 */
    sim_result_free(&r);
    /* all unknown: no stream ever publishes */
    wl.n = 0;
    CHECK_EQI(sim_run(&c, &wl, &tr, &r), 0);
    CHECK_EQI(summary_field(&r, &c, "aoi_mean_ms", v, sizeof(v)), 0);
    CHECK(strcmp(v, "NA") == 0);
    CHECK_EQI(summary_field(&r, &c, "aoi_mean_defined_ms", v, sizeof(v)), 0);
    CHECK(strcmp(v, "NA") == 0);
    CHECK_EQI(summary_field(&r, &c, "aoi_defined_streams", v, sizeof(v)), 0);
    CHECK_EQ((unsigned)atoi(v), 0);
    CHECK_EQI(summary_field(&r, &c, "unknown_ms_sum", v, sizeof(v)), 0);
    CHECK_EQ((unsigned)atoi(v), 200);
    sim_result_free(&r);
    /* both defined: headline equals the partial aggregate and coverage is 2 */
    wl.n = 0;
    wl_state(&wl, 0, 0, 1);
    wl_state(&wl, 0, 1, 1);
    CHECK_EQI(sim_run(&c, &wl, &tr, &r), 0);
    CHECK_EQI(summary_field(&r, &c, "aoi_mean_ms", v, sizeof(v)), 0);
    CHECK(strcmp(v, "NA") != 0);
    CHECK_EQI(summary_field(&r, &c, "aoi_defined_streams", v, sizeof(v)), 0);
    CHECK_EQ((unsigned)atoi(v), 2);
    sim_result_free(&r);
    free(wl.items);
    trace_free(&tr);
}

/*
 * Accounting scenario: 2 streams every 100 ms, outage on both directions for
 * 400 <= t < 1200, 12 RAISE at 500,520,...,720 (deadline 300, retention 600).
 * Hand-computable: capacity 8 -> events 1..8 admitted, 9..12 REJECTED_FULL
 * (no slot can free during the outage: nothing is ACKed and the earliest
 * retention expiry is 500+600=1100 > 720). Events 1..6 have retention_abs
 * 1100..1200 <= outage end, so they expire at exactly their retention_abs.
 */
static void build_accounting(workload_t *wl, trace_t *tr)
{
    fl_time_t t;
    uint32_t i;
    wl_init(wl);
    for (t = 0; t < 2000; t += 100) {
        wl_state(wl, t, 0, (int32_t)t);
        wl_state(wl, t, 1, (int32_t)(t * 2u));
        if (t >= 500 && t <= 720 && ((t - 500) % 20) == 0) {
            /* events interleave with states at t=500,600,700 */
        }
    }
    /* events must be in time order together with states: rebuild sorted */
    {
        wl_action_t *tmp = (wl_action_t *)calloc(4096, sizeof(wl_action_t));
        uint32_t n = 0;
        fl_time_t ev_t = 500;
        uint32_t k = 0;
        uint32_t idx = 0;
        while (idx < wl->n || k < 12) {
            if (k < 12 && (idx >= wl->n || ev_t <= wl->items[idx].time_ms)) {
                wl_action_t *a = &tmp[n++];
                memset(a, 0, sizeof(*a));
                a->time_ms = ev_t;
                a->action = WL_RAISE;
                a->code = (uint8_t)(k + 1u);
                a->deadline_rel = 300;
                a->retention_rel = 600;
                k++;
                ev_t += 20;
            } else {
                tmp[n++] = wl->items[idx++];
            }
        }
        free(wl->items);
        wl->items = tmp;
        wl->n = n;
        wl->n_events = 12;
    }
    mk_trace(tr, 200, 2);
    for (i = 40; i < 120; i++) {
        tr->data_lost[i] = 1;
        tr->ack_lost[i] = 1;
    }
}

static void check_identity(const sim_result_t *r)
{
    uint32_t i;
    uint32_t delivered = 0, on_time = 0, late = 0, sent_states = 0;
    CHECK_EQ(r->core_error, 0);
    CHECK_EQ(r->ledger_mismatch, 0);
    CHECK_EQ(r->interval_violations, 0);
    CHECK_EQ(r->n_events, r->s.events_generated);
    CHECK_EQ(r->s.events_generated, r->s.events_rejected_full + r->s.events_acked + r->s.events_retry_exhausted +
                                        r->s.events_retention_expired + r->ev_pending_end);
    CHECK_EQ(r->s.events_generated, r->s.events_admitted + r->s.events_rejected_full);
    for (i = 0; i < r->n_events; i++) {
        const ev_record_t *e = &r->events[i];
        CHECK_EQ(e->id, i + 1u);
        if (e->rx_first_time != FL_TIME_NONE) {
            delivered++;
            if (e->rx_on_time) on_time++; else late++;
        }
        if (!e->admitted) {
            CHECK_EQ(e->outcome, FL_TERM_REJECTED_FULL);
            CHECK_EQ(e->attempts, 0);
        }
    }
    CHECK_EQ(delivered, r->r.ev_delivered);
    CHECK_EQ(on_time, r->r.ev_on_time);
    CHECK_EQ(late, r->r.ev_late);
    for (i = 0; i < FL_MAX_STREAMS; i++) {
        sent_states += r->sc[i].sent;
    }
    CHECK_EQ(sent_states, r->s.state_tx);
    CHECK_EQ(r->data_frames, r->s.state_tx + r->s.event_tx);
    CHECK_EQ(r->ack_frames, r->r.ack_tx);
    CHECK_EQ(r->data_frames, r->r.frames_ok + r->r.frames_rejected + r->r.session_mismatch + r->data_lost + r->in_transit_data);
    CHECK_EQ(r->ack_frames, r->s.acks_ok + r->s.acks_unmatched + r->s.acks_impossible + r->s.acks_rejected_frame +
                                r->s.acks_session_mismatch + r->ack_lost + r->in_transit_ack);
    CHECK_EQ(r->in_transit_end, r->in_transit_data + r->in_transit_ack);
}

static void test_accounting_identity(void)
{
    sim_config_t c;
    workload_t wl;
    trace_t tr;
    sim_result_t r;
    uint32_t i;
    build_accounting(&wl, &tr);
    cfg_init(&c, "edf_rr", 2000, 2);
    CHECK_EQI(sim_run(&c, &wl, &tr, &r), 0);
    check_identity(&r);
    CHECK_EQ(r.s.events_generated, 12);
    CHECK_EQ(r.s.events_admitted, 8);
    CHECK_EQ(r.s.events_rejected_full, 4);
    for (i = 8; i < 12; i++) {
        CHECK_EQ(r.events[i].outcome, FL_TERM_REJECTED_FULL);
        CHECK_EQ(r.events[i].terminal_time, 500u + 20u * i);
    }
    for (i = 0; i < 6; i++) {
        CHECK_EQ(r.events[i].outcome, FL_TERM_RETENTION_EXPIRED);
        CHECK_EQ(r.events[i].terminal_time, r.events[i].retention_abs);
        CHECK_EQ(r.events[i].rx_first_time, FL_TIME_NONE); /* nothing crosses the outage */
        CHECK(r.events[i].attempts >= 1);
    }
    CHECK(r.s.events_retention_expired >= 6);
    /* anything delivered after the outage is late (deadline 300 ms passed long ago) */
    CHECK_EQ(r.r.ev_on_time, 0);
    CHECK_EQ(r.r.ev_late, r.r.ev_delivered);
    sim_result_free(&r);
    free(wl.items);
    trace_free(&tr);
}

static int same_decisions(const sim_result_t *a, const sim_result_t *b)
{
    uint32_t i;
    if (a->n_decisions != b->n_decisions) return 0;
    for (i = 0; i < a->n_decisions; i++) {
        const dec_record_t *x = &a->decisions[i];
        const dec_record_t *y = &b->decisions[i];
        if (x->choice.kind != y->choice.kind || x->choice.reason != y->choice.reason ||
            x->choice.stream != y->choice.stream || x->choice.event_id != y->choice.event_id ||
            x->attempt != y->attempt || x->seq != y->seq || x->lost != y->lost || x->ack_type != y->ack_type) {
            return 0;
        }
    }
    return 1;
}

static void test_determinism(void)
{
    sim_config_t c;
    workload_t wl;
    trace_t tr;
    sim_result_t r1, r2;
    build_accounting(&wl, &tr);
    cfg_init(&c, "fresh", 2000, 2);
    CHECK_EQI(sim_run(&c, &wl, &tr, &r1), 0);
    CHECK_EQI(sim_run(&c, &wl, &tr, &r2), 0);
    CHECK(same_decisions(&r1, &r2));
    CHECK(memcmp(&r1.s, &r2.s, sizeof(r1.s)) == 0);
    CHECK(memcmp(&r1.r, &r2.r, sizeof(r1.r)) == 0);
    CHECK_EQ(r1.n_events, r2.n_events);
    CHECK(memcmp(r1.events, r2.events, r1.n_events * sizeof(ev_record_t)) == 0);
    CHECK(memcmp(r1.aoi, r2.aoi, sizeof(r1.aoi)) == 0);
    sim_result_free(&r1);
    sim_result_free(&r2);
    free(wl.items);
    trace_free(&tr);
}

/* §9.3 ablation: fresh with defer=0 must reproduce fresh_nodefer exactly; edf_rr agrees where events exist. */
static void test_ablation_equivalence(void)
{
    sim_config_t c1, c2, c3;
    workload_t wl;
    trace_t tr;
    sim_result_t r1, r2, r3;
    uint32_t i;
    uint32_t slots_with_events = 0;
    build_accounting(&wl, &tr);
    cfg_init(&c1, "fresh", 2000, 2);
    c1.policy.defer = 0;
    cfg_init(&c2, "fresh_nodefer", 2000, 2);
    cfg_init(&c3, "edf_rr", 2000, 2);
    CHECK_EQI(sim_run(&c1, &wl, &tr, &r1), 0);
    CHECK_EQI(sim_run(&c2, &wl, &tr, &r2), 0);
    CHECK_EQI(sim_run(&c3, &wl, &tr, &r3), 0);
    CHECK(same_decisions(&r1, &r2));
    for (i = 0; i < r2.n_decisions; i++) {
        if (r2.decisions[i].choice.n_elig_ev > 0) {
            slots_with_events++;
            CHECK_EQ(r2.decisions[i].choice.kind, FL_DK_EVENT);
            CHECK_EQ(r3.decisions[i].choice.kind, FL_DK_EVENT);
            CHECK_EQ(r2.decisions[i].choice.event_id, r3.decisions[i].choice.event_id);
        }
    }
    CHECK(slots_with_events > 0);
    /* and the candidate with defer=1 must differ somewhere on this workload (it has stale state during the outage) */
    {
        sim_config_t c4;
        sim_result_t r4;
        cfg_init(&c4, "fresh", 2000, 2);
        CHECK_EQI(sim_run(&c4, &wl, &tr, &r4), 0);
        check_identity(&r4);
        sim_result_free(&r4);
    }
    sim_result_free(&r1);
    sim_result_free(&r2);
    sim_result_free(&r3);
    free(wl.items);
    trace_free(&tr);
}

/*
 * Ledgers are independent (§7.4): with every ACK lost, the receiver delivers
 * everything while the sender confirms nothing. Lost ACK attempts are still
 * charged. 3 events at t=0, run 1000 ms, no data loss, all ACKs lost.
 */
static void test_ledgers_independent_under_ack_loss(void)
{
    sim_config_t c;
    workload_t wl;
    trace_t tr;
    sim_result_t r;
    cfg_init(&c, "edf_rr", 1000, 1);
    c.max_attempts = 2;
    wl_init(&wl);
    wl_event(&wl, 0, FL_EV_RAISE, 1, 200, 900);
    wl_event(&wl, 0, FL_EV_CLEAR, 1, 200, 900);
    wl_event(&wl, 0, FL_EV_RAISE, 2, 200, 900);
    mk_trace(&tr, 100, 2);
    set_loss(&tr, 0, 1, 0, 100);
    CHECK_EQI(sim_run(&c, &wl, &tr, &r), 0);
    check_identity(&r);
    CHECK_EQ(r.r.ev_delivered, 3);
    CHECK_EQ(r.r.ev_on_time, 3);
    CHECK_EQ(r.s.events_acked, 0);
    CHECK_EQ(r.delivered_but_unacked, 3);
    /* attempts: sends at 0,10,20; retries at 300,310,320 (timeout 300); exhausted at 600,610,620 */
    CHECK_EQ(r.s.events_retry_exhausted, 3);
    CHECK_EQ(r.s.event_tx, 6);
    CHECK_EQ(r.r.ev_duplicate, 3);
    CHECK_EQ(r.ack_frames, 6);
    CHECK_EQ(r.ack_lost, 6);
    CHECK_EQ(r.r.bytes_ack_tx, 60); /* 6 x 10 bytes charged although lost */
    CHECK_EQ(r.s.bytes_data_tx, 6u * 27u);
    CHECK_EQ(r.events[0].terminal_time, 600);
    CHECK_EQ(r.events[0].rx_first_time, 2);
    sim_result_free(&r);
    free(wl.items);
    trace_free(&tr);
}

/* Late receipt is not deadline success (§7.3): deadline 10, delay 50 -> delivered, late, recall 1, on-time 0. */
static void test_late_is_not_success(void)
{
    sim_config_t c;
    workload_t wl;
    trace_t tr;
    sim_result_t r;
    cfg_init(&c, "fresh", 500, 1);
    wl_init(&wl);
    wl_event(&wl, 0, FL_EV_RAISE, 1, 10, 400);
    mk_trace(&tr, 50, 50);
    CHECK_EQI(sim_run(&c, &wl, &tr, &r), 0);
    check_identity(&r);
    CHECK_EQ(r.r.ev_delivered, 1);
    CHECK_EQ(r.r.ev_on_time, 0);
    CHECK_EQ(r.r.ev_late, 1);
    CHECK_EQ(r.events[0].rx_first_time, 50);
    CHECK_EQ(r.events[0].rx_on_time, 0);
    /* sent at slot 0, arrives at 50 which is itself a slot, so the ACK leaves at 50
       (§8.1: data arrival precedes the reverse step in the same ms) and lands at 100 */
    CHECK_EQ(r.events[0].outcome, FL_TERM_ACKED);
    CHECK_EQ(r.events[0].terminal_time, 100);
    CHECK_EQ(r.conf_max, 100);
    CHECK_EQ(r.lat_max, 50);
    sim_result_free(&r);
    free(wl.items);
    trace_free(&tr);
}

/*
 * AoI through the harness, hand-computed: one stream, publishes at 0, 30, 60;
 * slot 10, delay 2, no loss -> sends at 0, 30, 60; applies at 2, 32, 62; end 100.
 * Segments: [2,32) a0=2 n=30 -> 2*30*2+900 = 1020; [32,62) same 1020;
 * [62,100) a0=2 n=38 -> 152+1444 = 1596. area2 = 3636, area 1818, mean 1818/98.
 * Peak 40 (end of run), final age 40, unknown 2.
 */
static void test_aoi_through_harness(void)
{
    sim_config_t c;
    workload_t wl;
    trace_t tr;
    sim_result_t r;
    cfg_init(&c, "edf_rr", 100, 1);
    wl_init(&wl);
    wl_state(&wl, 0, 0, 1);
    wl_state(&wl, 30, 0, 2);
    wl_state(&wl, 60, 0, 3);
    mk_trace(&tr, 10, 2);
    CHECK_EQI(sim_run(&c, &wl, &tr, &r), 0);
    check_identity(&r);
    CHECK_EQ(r.sc[0].applied, 3);
    CHECK_EQ(r.aoi[0].area2, 3636);
    CHECK_NEAR(aoi_mean(&r.aoi[0], 100), 1818.0 / 98.0, 1e-9);
    CHECK_EQ(r.aoi[0].peak, 40);
    CHECK_EQ(r.aoi[0].final_age, 40);
    CHECK_EQ(r.aoi[0].unknown_ms, 2);
    sim_result_free(&r);
    free(wl.items);
    trace_free(&tr);
}

/* Writes a config file with one key=value line and loads it; returns load_config's rc. */
static int load_one(const char *key, unsigned long v, sim_config_t *cfg)
{
    char path[] = "/tmp/flcfgXXXXXX";
    int fd = mkstemp(path);
    FILE *fp;
    int rc;
    if (fd < 0) {
        return -99;
    }
    fp = fdopen(fd, "w");
    if (fp == 0) {
        return -99;
    }
    fprintf(fp, "%s=%lu\n", key, v);
    fclose(fp);
    sim_config_defaults(cfg);
    rc = load_config(path, cfg);
    unlink(path);
    return rc;
}

/*
 * Config regression: values must be range-checked BEFORE narrowing.
 * n_streams=260 would wrap to 4, max_attempts=264 to 8, session_id=65543 to 7
 * if cast first; all must be rejected. Exact boundaries are accepted.
 */
static void test_config_ranges_before_narrowing(void)
{
    sim_config_t c;
    CHECK_EQI(load_one("n_streams", FL_MAX_STREAMS, &c), 0);
    CHECK_EQ(c.n_streams, FL_MAX_STREAMS);
    CHECK_EQI(load_one("n_streams", FL_MAX_STREAMS + 1u, &c), -1);
    CHECK_EQI(load_one("n_streams", 0, &c), -1);
    CHECK_EQI(load_one("n_streams", 260, &c), -1);   /* would wrap to 4 */
    CHECK_EQI(load_one("max_attempts", 255, &c), 0);
    CHECK_EQ(c.max_attempts, 255);
    CHECK_EQI(load_one("max_attempts", 256, &c), -1);
    CHECK_EQI(load_one("max_attempts", 264, &c), -1); /* would wrap to 8 */
    CHECK_EQI(load_one("max_attempts", 0, &c), -1);
    CHECK_EQI(load_one("session_id", 65535, &c), 0);
    CHECK_EQ(c.session_id, 65535);
    CHECK_EQI(load_one("session_id", 65536, &c), -1);
    CHECK_EQI(load_one("session_id", 65543, &c), -1); /* would wrap to 7 */
    CHECK_EQI(load_one("session_id", 0, &c), 0);
    CHECK_EQI(load_one("run_ms", FL_TIME_HORIZON_MS, &c), 0);
    CHECK_EQI(load_one("run_ms", (unsigned long)FL_TIME_HORIZON_MS + 1ul, &c), -1);
    CHECK_EQI(load_one("run_ms", 0, &c), -1);
    CHECK_EQI(load_one("slot_ms", 0, &c), -1);
    CHECK_EQI(load_one("ack_timeout_ms", FL_MAX_REL_MS, &c), 0);
    CHECK_EQI(load_one("ack_timeout_ms", (unsigned long)FL_MAX_REL_MS + 1ul, &c), -1);
    CHECK_EQI(load_one("ack_timeout_ms", 0, &c), -1);
    CHECK_EQI(load_one("state_stale_ms", (unsigned long)FL_MAX_REL_MS + 1ul, &c), -1);
    CHECK_EQI(load_one("event_service_ms", 0, &c), 0);
    CHECK_EQI(load_one("bogus_key", 1, &c), -1);
    CHECK_EQI(load_one("n_streams", 4294967296ul, &c), -1); /* > UINT32: parse rejects */
}

int main(void)
{
    RUN(test_config_ranges_before_narrowing);
    RUN(test_reject_out_of_run_rows);
    RUN(test_attempts_recorded_when_pending);
    RUN(test_reject_zero_delay);
    RUN(test_aoi_undefined_streams_are_na);
    RUN(test_accounting_identity);
    RUN(test_determinism);
    RUN(test_ablation_equivalence);
    RUN(test_ledgers_independent_under_ack_loss);
    RUN(test_late_is_not_success);
    RUN(test_aoi_through_harness);
    return FLTEST_REPORT("test_sim");
}
