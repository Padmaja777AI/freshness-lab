/* Freshness Lab — scheduling policies. SPDX-License-Identifier: MIT */
#include "fl_policy.h"

/* ---- shared definitions (§9.0) ---------------------------------------- */

uint8_t fl_policy_event_is_late(const fl_sched_view_t *view, const fl_view_event_t *e)
{
    return (e->deadline_abs < view->now) ? 1u : 0u;
}

static fl_time_t remaining_ms(const fl_sched_view_t *view, const fl_view_event_t *e)
{
    return (e->deadline_abs > view->now) ? (e->deadline_abs - view->now) : 0u;
}

/* est_aoi(s): sender-side freshness estimate; FL_TIME_NONE == infinite. */
static fl_time_t est_aoi(const fl_sched_view_t *view, const fl_view_stream_t *s)
{
    if (s->last_acked_gen == FL_TIME_NONE) {
        return FL_TIME_NONE;
    }
    return view->now - s->last_acked_gen;
}

static fl_time_t since_tx(const fl_sched_view_t *view, const fl_view_stream_t *s)
{
    if (s->last_tx_time == FL_TIME_NONE) {
        return FL_TIME_NONE;
    }
    return view->now - s->last_tx_time;
}

/* Largest est_aoi, tie -> lowest stream id (streams are ascending). */
static const fl_view_stream_t *pick_fresh(const fl_sched_view_t *view)
{
    const fl_view_stream_t *best = 0;
    fl_time_t best_aoi = 0;
    uint8_t i;
    for (i = 0; i < view->n_streams; i++) {
        fl_time_t a = est_aoi(view, &view->streams[i]);
        if (best == 0 || a > best_aoi) {
            best = &view->streams[i];
            best_aoi = a;
        }
    }
    return best;
}

/* Round robin: first eligible stream at or after rr_next. */
static const fl_view_stream_t *pick_rr(const fl_sched_view_t *view, uint8_t *rr_next_out)
{
    uint8_t k;
    uint8_t n = view->n_streams_total;
    if (n == 0 || view->n_streams == 0) {
        return 0;
    }
    for (k = 0; k < n; k++) {
        uint8_t sid = (uint8_t)((view->rr_next + k) % n);
        uint8_t i;
        for (i = 0; i < view->n_streams; i++) {
            if (view->streams[i].stream == sid) {
                *rr_next_out = (uint8_t)((sid + 1u) % n);
                return &view->streams[i];
            }
        }
    }
    return 0;
}

static uint8_t is_starved(const fl_sched_view_t *view, const fl_view_stream_t *s)
{
    fl_time_t t = since_tx(view, s);
    return (t == FL_TIME_NONE || t >= view->params.state_starvation_ms) ? 1u : 0u;
}

/*
 * Among STARVED eligible streams, the one with the oldest last_tx_time
 * (never transmitted counts as oldest); tie -> lowest stream id. Returns 0
 * when no eligible stream is starved. This is deliberately a different
 * ranking from pick_fresh: the starvation branch must serve a stream that
 * actually triggered it (regression: A.last_tx=95/acked_gen=0 vs
 * B.last_tx=0/acked_gen=50 at now=100, threshold 30 -> B, not A).
 */
static const fl_view_stream_t *pick_starved(const fl_sched_view_t *view)
{
    const fl_view_stream_t *best = 0;
    fl_time_t best_since = 0;
    uint8_t i;
    for (i = 0; i < view->n_streams; i++) {
        const fl_view_stream_t *s = &view->streams[i];
        fl_time_t t;
        if (!is_starved(view, s)) {
            continue;
        }
        t = since_tx(view, s);
        if (best == 0 || t > best_since) {
            best = s;
            best_since = t;
        }
    }
    return best;
}

static uint8_t any_stale(const fl_sched_view_t *view)
{
    uint8_t i;
    for (i = 0; i < view->n_streams; i++) {
        fl_time_t a = est_aoi(view, &view->streams[i]);
        if (a == FL_TIME_NONE || a >= view->params.state_stale_ms) {
            return 1;
        }
    }
    return 0;
}

/*
 * Urgency with the declared service assumption: E[i] (EDF order, 0-based)
 * is urgent iff remaining <= (i+1)*event_service_ms + slack_guard_ms.
 * Computed in uint64 so no intermediate can wrap.
 */
static uint8_t any_urgent(const fl_sched_view_t *view, const fl_view_event_t *ev, uint8_t n)
{
    uint8_t i;
    for (i = 0; i < n; i++) {
        uint64_t budget = (uint64_t)(i + 1u) * (uint64_t)view->params.event_service_ms +
                          (uint64_t)view->params.slack_guard_ms;
        if ((uint64_t)remaining_ms(view, &ev[i]) <= budget) {
            return 1;
        }
    }
    return 0;
}

static void choose_event(const fl_sched_view_t *view, const fl_view_event_t *e, uint8_t reason,
                         fl_choice_t *out)
{
    out->kind = FL_DK_EVENT;
    out->event_id = e->id;
    out->reason = fl_policy_event_is_late(view, e) ? (uint8_t)FL_R_EVENT_LATE : reason;
}

static void choose_stream(const fl_view_stream_t *s, uint8_t reason, fl_choice_t *out)
{
    out->kind = FL_DK_STATE;
    out->stream = s->stream;
    out->reason = reason;
}

/* ---- families ---------------------------------------------------------- */

/*
 * Late demotion (§9.0) is applied identically for every family: the family
 * sees only fresh events (fe/nf); demoted late events (le/nl) are served
 * only when the family would otherwise idle.
 */
typedef struct {
    fl_view_event_t fe[FL_EVENT_CAPACITY];
    uint8_t nf;
    fl_view_event_t le[FL_EVENT_CAPACITY];
    uint8_t nl;
} split_t;

static void split_events(const fl_sched_view_t *view, split_t *sp)
{
    uint8_t i;
    sp->nf = 0;
    sp->nl = 0;
    for (i = 0; i < view->n_events; i++) {
        if (view->params.late_demote && fl_policy_event_is_late(view, &view->events[i])) {
            sp->le[sp->nl++] = view->events[i];
        } else {
            sp->fe[sp->nf++] = view->events[i];
        }
    }
}

static void select_edf_rr(const fl_sched_view_t *view, const split_t *sp, fl_choice_t *out,
                          uint8_t *rr_next_out)
{
    if (sp->nf > 0) {
        choose_event(view, &sp->fe[0], FL_R_EVENT_EDF, out);
        return;
    }
    if (view->n_streams > 0) {
        const fl_view_stream_t *s = pick_rr(view, rr_next_out);
        if (s != 0) {
            choose_stream(s, FL_R_STATE_RR, out);
            return;
        }
    }
}

/* Ablation baseline (§9.2): written separately on purpose. */
static void select_fresh_nodefer(const fl_sched_view_t *view, const split_t *sp, fl_choice_t *out)
{
    if (sp->nf > 0) {
        choose_event(view, &sp->fe[0], FL_R_EVENT_EDF, out);
        return;
    }
    if (view->n_streams > 0) {
        choose_stream(pick_fresh(view), FL_R_STATE_FRESH, out);
        return;
    }
}

/* Candidate (§9.3) and fresh_so (§9.3b). */
static void select_fresh(const fl_sched_view_t *view, const split_t *sp, fl_choice_t *out)
{
    if (sp->nf > 0 && view->n_streams > 0 && view->params.defer) {
        uint8_t urgent = any_urgent(view, sp->fe, sp->nf);
        const fl_view_stream_t *starved = pick_starved(view);
        if (view->params.starvation_override) {
            if (starved != 0) {
                choose_stream(starved, FL_R_STATE_STARVATION, out);
                return;
            }
            if (urgent) {
                choose_event(view, &sp->fe[0], FL_R_EVENT_URGENT, out);
                return;
            }
        } else {
            if (urgent) {
                choose_event(view, &sp->fe[0], FL_R_EVENT_URGENT, out);
                return;
            }
            if (starved != 0) {
                choose_stream(starved, FL_R_STATE_STARVATION, out);
                return;
            }
        }
        if (any_stale(view)) {
            choose_stream(pick_fresh(view), FL_R_STATE_STALE, out);
            return;
        }
        choose_event(view, &sp->fe[0], FL_R_EVENT_SLACK_OK, out);
        return;
    }
    if (sp->nf > 0) {
        choose_event(view, &sp->fe[0], FL_R_EVENT_EDF, out);
        return;
    }
    if (view->n_streams > 0) {
        choose_stream(pick_fresh(view), FL_R_STATE_FRESH, out);
        return;
    }
}

/* FIFO (§9.4): smallest gen_time; tie -> event before state, then lower id. */
static void select_fifo(const fl_sched_view_t *view, const split_t *sp, fl_choice_t *out)
{
    const fl_view_event_t *be = 0;
    const fl_view_stream_t *bs = 0;
    uint8_t i;
    for (i = 0; i < sp->nf; i++) {
        if (be == 0 || sp->fe[i].gen_time < be->gen_time ||
            (sp->fe[i].gen_time == be->gen_time && sp->fe[i].id < be->id)) {
            be = &sp->fe[i];
        }
    }
    for (i = 0; i < view->n_streams; i++) {
        if (bs == 0 || view->streams[i].latest_gen < bs->latest_gen) {
            bs = &view->streams[i];
        }
    }
    if (be != 0 && (bs == 0 || be->gen_time <= bs->latest_gen)) {
        choose_event(view, be, FL_R_FIFO_OLDEST, out);
        return;
    }
    if (bs != 0) {
        choose_stream(bs, FL_R_FIFO_OLDEST, out);
        return;
    }
}

int fl_policy_select(const fl_sched_view_t *view, fl_choice_t *out, uint8_t *rr_next_out)
{
    split_t sp;
    if (view == 0 || out == 0 || rr_next_out == 0) {
        return FL_ERR_ARG;
    }
    if (view->n_events > FL_EVENT_CAPACITY || view->n_streams > FL_MAX_STREAMS ||
        view->n_streams_total > FL_MAX_STREAMS) {
        return FL_ERR_ARG;
    }
    out->kind = FL_DK_NONE;
    out->reason = FL_R_IDLE;
    out->stream = 0;
    out->event_id = 0;
    out->n_elig_ev = view->n_events;
    out->n_elig_st = view->n_streams;
    *rr_next_out = view->rr_next;

    split_events(view, &sp);

    switch (view->params.family) {
    case FL_POL_EDF_RR: select_edf_rr(view, &sp, out, rr_next_out); break;
    case FL_POL_FRESH_NODEFER: select_fresh_nodefer(view, &sp, out); break;
    case FL_POL_FRESH: select_fresh(view, &sp, out); break;
    case FL_POL_FIFO: select_fifo(view, &sp, out); break;
    default: return FL_ERR_ARG;
    }

    if (out->kind == FL_DK_NONE && sp.nl > 0) {
        /* Demoted late events: EDF order among them (§9.0). */
        out->kind = FL_DK_EVENT;
        out->event_id = sp.le[0].id;
        out->reason = FL_R_EVENT_LATE_DEMOTED;
    }
    return FL_OK;
}

/* ---- names ------------------------------------------------------------- */

static int str_eq(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

const char *fl_policy_family_name(uint8_t family)
{
    switch (family) {
    case FL_POL_EDF_RR: return "edf_rr";
    case FL_POL_FRESH_NODEFER: return "fresh_nodefer";
    case FL_POL_FRESH: return "fresh";
    case FL_POL_FIFO: return "fifo";
    default: return "?";
    }
}

/*
 * Named presets (§9.5). Only family/defer/late_demote/starvation_override are
 * set here; timing parameters come from the config file.
 */
int fl_policy_family_from_name(const char *name, fl_policy_params_t *p)
{
    if (name == 0 || p == 0) {
        return FL_ERR_ARG;
    }
    p->defer = 0;
    p->late_demote = 0;
    p->starvation_override = 0;
    if (str_eq(name, "edf_rr")) {
        p->family = FL_POL_EDF_RR;
    } else if (str_eq(name, "edf_rr_ld")) {
        p->family = FL_POL_EDF_RR;
        p->late_demote = 1;
    } else if (str_eq(name, "fresh_nodefer")) {
        p->family = FL_POL_FRESH_NODEFER;
    } else if (str_eq(name, "fresh_nodefer_ld")) {
        p->family = FL_POL_FRESH_NODEFER;
        p->late_demote = 1;
    } else if (str_eq(name, "fresh")) {
        p->family = FL_POL_FRESH;
        p->defer = 1;
    } else if (str_eq(name, "fresh_ld")) {
        p->family = FL_POL_FRESH;
        p->defer = 1;
        p->late_demote = 1;
    } else if (str_eq(name, "fresh_so")) {
        p->family = FL_POL_FRESH;
        p->defer = 1;
        p->starvation_override = 1;
    } else if (str_eq(name, "fifo")) {
        p->family = FL_POL_FIFO;
    } else if (str_eq(name, "fifo_ld")) {
        p->family = FL_POL_FIFO;
        p->late_demote = 1;
    } else {
        return FL_ERR_ARG;
    }
    return FL_OK;
}
