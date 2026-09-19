/*
 * Scheduling policies on hand-built views (docs/DESIGN.md §9).
 * Every expectation is derived from the §9 rules by hand; comments show the
 * arithmetic. No fl_sender_t is involved.
 * SPDX-License-Identifier: MIT
 */
#include "fltest.h"
#include "../core/fl_policy.h"

static fl_sched_view_t V;

static void vinit(const char *preset, fl_time_t now)
{
    memset(&V, 0, sizeof(V));
    CHECK_EQI(fl_policy_family_from_name(preset, &V.params), FL_OK);
    V.params.event_service_ms = 40;
    V.params.slack_guard_ms = 100;
    V.params.state_stale_ms = 1000;
    V.params.state_starvation_ms = 2000;
    V.now = now;
    V.n_streams_total = 4;
}

/* Events must be appended in (deadline_abs, id) order, as the sender does. */
static void vev(uint32_t id, fl_time_t deadline_abs, fl_time_t gen)
{
    fl_view_event_t *e = &V.events[V.n_events++];
    e->id = id;
    e->deadline_abs = deadline_abs;
    e->gen_time = gen;
}

static void vst(uint8_t stream, fl_time_t latest_gen, fl_time_t last_tx, fl_time_t acked_gen)
{
    fl_view_stream_t *s = &V.streams[V.n_streams++];
    s->stream = stream;
    s->latest_gen = latest_gen;
    s->last_tx_time = last_tx;
    s->last_acked_gen = acked_gen;
}

static fl_choice_t sel(uint8_t *rr_out)
{
    fl_choice_t c;
    uint8_t rr = 0xFF;
    CHECK_EQI(fl_policy_select(&V, &c, &rr), FL_OK);
    if (rr_out) {
        *rr_out = rr;
    }
    return c;
}

static void test_edf_rr(void)
{
    fl_choice_t c;
    uint8_t rr;
    /* two events, same deadline: sorted (1,50),(2,50) -> id 1, EVENT_EDF */
    vinit("edf_rr", 10);
    vev(1, 50, 0);
    vev(2, 50, 0);
    vst(0, 5, FL_TIME_NONE, FL_TIME_NONE);
    c = sel(&rr);
    CHECK_EQ(c.kind, FL_DK_EVENT);
    CHECK_EQ(c.event_id, 1);
    CHECK_EQ(c.reason, FL_R_EVENT_EDF);
    CHECK_EQ(c.n_elig_ev, 2);
    CHECK_EQ(c.n_elig_st, 1);
    /* late: deadline 40 < now 50 -> EVENT_LATE, still first */
    vinit("edf_rr", 50);
    vev(3, 40, 0);
    vst(0, 5, FL_TIME_NONE, FL_TIME_NONE);
    c = sel(&rr);
    CHECK_EQ(c.kind, FL_DK_EVENT);
    CHECK_EQ(c.event_id, 3);
    CHECK_EQ(c.reason, FL_R_EVENT_LATE);
    /* streams only: rr_next 1 -> stream 1, cursor 2 */
    vinit("edf_rr", 100);
    vst(0, 1, 0, 0);
    vst(1, 1, 0, 0);
    vst(2, 1, 0, 0);
    V.rr_next = 1;
    c = sel(&rr);
    CHECK_EQ(c.kind, FL_DK_STATE);
    CHECK_EQ(c.stream, 1);
    CHECK_EQ(c.reason, FL_R_STATE_RR);
    CHECK_EQ(rr, 2);
    /* rr wraps past an ineligible stream: eligible {0,1}, total 3, rr_next 2 -> stream 0, cursor 1 */
    vinit("edf_rr", 100);
    V.n_streams_total = 3;
    vst(0, 1, 0, 0);
    vst(1, 1, 0, 0);
    V.rr_next = 2;
    c = sel(&rr);
    CHECK_EQ(c.kind, FL_DK_STATE);
    CHECK_EQ(c.stream, 0);
    CHECK_EQ(rr, 1);
    /* the cursor is untouched when an event is chosen */
    vinit("edf_rr", 100);
    vev(1, 500, 0);
    vst(0, 1, 0, 0);
    V.rr_next = 3;
    c = sel(&rr);
    CHECK_EQ(c.kind, FL_DK_EVENT);
    CHECK_EQ(rr, 3);
}

static void test_fresh_nodefer(void)
{
    fl_choice_t c;
    uint8_t rr;
    /* event first regardless of state staleness */
    vinit("fresh_nodefer", 100);
    vev(1, 100000, 0);
    vst(0, 1, FL_TIME_NONE, FL_TIME_NONE);
    c = sel(&rr);
    CHECK_EQ(c.kind, FL_DK_EVENT);
    CHECK_EQ(c.reason, FL_R_EVENT_EDF);
    /* est_aoi = now - acked_gen: 60 vs 90 -> stream 1 */
    vinit("fresh_nodefer", 100);
    vst(0, 90, 50, 40);
    vst(1, 90, 50, 10);
    V.rr_next = 0;
    c = sel(&rr);
    CHECK_EQ(c.kind, FL_DK_STATE);
    CHECK_EQ(c.stream, 1);
    CHECK_EQ(c.reason, FL_R_STATE_FRESH);
    CHECK_EQ(rr, 0); /* freshness ranking never moves the RR cursor */
    /* NONE (never acked) beats any finite estimate */
    vinit("fresh_nodefer", 100);
    vst(0, 90, 50, 40);
    vst(1, 90, 50, FL_TIME_NONE);
    c = sel(&rr);
    CHECK_EQ(c.stream, 1);
    /* tie -> lowest id */
    vinit("fresh_nodefer", 100);
    vst(2, 90, 50, 40);
    vst(3, 90, 50, 40);
    c = sel(&rr);
    CHECK_EQ(c.stream, 2);
}

/*
 * Urgency with the (i+1) term: service 40, guard 100 -> budgets 140, 180, 220.
 * Deadlines 1150, 1160, 1400 at now 1000: remaining 150 > 140 (E0 not urgent
 * alone) but 160 <= 180 (E1 urgent by queue depth) -> choose E0, EVENT_URGENT.
 */
static void test_fresh_urgency_queue_term(void)
{
    fl_choice_t c;
    vinit("fresh", 1000);
    vev(1, 1150, 900);
    vev(2, 1160, 900);
    vev(3, 1400, 900);
    vst(0, 990, 990, FL_TIME_NONE); /* stale (never acked) so deferral would be tempting */
    c = sel(0);
    CHECK_EQ(c.kind, FL_DK_EVENT);
    CHECK_EQ(c.event_id, 1);
    CHECK_EQ(c.reason, FL_R_EVENT_URGENT);
    /* deadlines 1150, 1400, 1500: 150>140, 400>180, 500>220 -> nothing urgent; stream stale -> STATE_STALE */
    vinit("fresh", 1000);
    vev(1, 1150, 900);
    vev(2, 1400, 900);
    vev(3, 1500, 900);
    vst(0, 990, 990, FL_TIME_NONE);
    c = sel(0);
    CHECK_EQ(c.kind, FL_DK_STATE);
    CHECK_EQ(c.stream, 0);
    CHECK_EQ(c.reason, FL_R_STATE_STALE);
    /* same events, stream acked 100 ms ago (aoi 100 < 1000) and sent recently -> EVENT_SLACK_OK */
    vinit("fresh", 1000);
    vev(1, 1150, 900);
    vev(2, 1400, 900);
    vst(0, 990, 990, 900);
    c = sel(0);
    CHECK_EQ(c.kind, FL_DK_EVENT);
    CHECK_EQ(c.event_id, 1);
    CHECK_EQ(c.reason, FL_R_EVENT_SLACK_OK);
    /* stale boundary: aoi == state_stale_ms counts as stale (>=) */
    vinit("fresh", 2000);
    vev(1, 2400, 1900);
    vst(0, 1990, 1990, 1000);
    c = sel(0);
    CHECK_EQ(c.reason, FL_R_STATE_STALE);
    vinit("fresh", 2000);
    vev(1, 2400, 1900);
    vst(0, 1990, 1990, 1001);
    c = sel(0);
    CHECK_EQ(c.reason, FL_R_EVENT_SLACK_OK);
    /* urgent boundary: remaining == 140 is urgent (<=) */
    vinit("fresh", 1000);
    vev(1, 1140, 900);
    vst(0, 990, 990, FL_TIME_NONE);
    c = sel(0);
    CHECK_EQ(c.reason, FL_R_EVENT_URGENT);
    /* a late event (deadline < now) is urgent and reported as EVENT_LATE */
    vinit("fresh", 1000);
    vev(1, 999, 900);
    vst(0, 990, 990, FL_TIME_NONE);
    c = sel(0);
    CHECK_EQ(c.kind, FL_DK_EVENT);
    CHECK_EQ(c.reason, FL_R_EVENT_LATE);
}

/*
 * Starvation ranking regression (§9.0): now 100, threshold 30.
 * A: last_tx 95 (since 5, not starved), acked_gen 0 (aoi 100).
 * B: last_tx 0 (since 100, starved), acked_gen 50 (aoi 50).
 * Event not urgent (remaining 10000). Must serve B with STATE_STARVATION.
 */
static void test_starvation_ranking_regression(void)
{
    fl_choice_t c;
    vinit("fresh", 100);
    V.params.state_starvation_ms = 30;
    vev(1, 10100, 0);
    vst(0, 90, 95, 0);
    vst(1, 90, 0, 50);
    c = sel(0);
    CHECK_EQ(c.kind, FL_DK_STATE);
    CHECK_EQ(c.stream, 1);
    CHECK_EQ(c.reason, FL_R_STATE_STARVATION);
    /* both starved: never-transmitted A (NONE) is oldest -> A */
    vinit("fresh", 100);
    V.params.state_starvation_ms = 30;
    vev(1, 10100, 0);
    vst(0, 90, FL_TIME_NONE, 0);
    vst(1, 90, 0, 50);
    c = sel(0);
    CHECK_EQ(c.stream, 0);
    CHECK_EQ(c.reason, FL_R_STATE_STARVATION);
    /* tie in since_tx -> lowest id */
    vinit("fresh", 100);
    V.params.state_starvation_ms = 30;
    vev(1, 10100, 0);
    vst(2, 90, 0, 90);
    vst(3, 90, 0, 0);
    c = sel(0);
    CHECK_EQ(c.stream, 2);
    /* starvation boundary: since_tx == threshold counts (>=) */
    vinit("fresh", 100);
    V.params.state_starvation_ms = 30;
    vev(1, 10100, 0);
    vst(0, 90, 70, 90);
    c = sel(0);
    CHECK_EQ(c.reason, FL_R_STATE_STARVATION);
    vinit("fresh", 100);
    V.params.state_starvation_ms = 30;
    vev(1, 10100, 0);
    vst(0, 90, 71, 90);
    c = sel(0);
    CHECK_EQ(c.reason, FL_R_EVENT_SLACK_OK);
}

/* fresh_so: a starved stream outranks an urgent event; fresh: the event wins. */
static void test_starvation_override(void)
{
    fl_choice_t c;
    vinit("fresh_so", 3000);
    vev(1, 3050, 2900); /* remaining 50 <= 140: urgent */
    vst(0, 2990, 500, 2900); /* since_tx 2500 >= 2000: starved */
    c = sel(0);
    CHECK_EQ(c.kind, FL_DK_STATE);
    CHECK_EQ(c.stream, 0);
    CHECK_EQ(c.reason, FL_R_STATE_STARVATION);
    CHECK_EQ(V.params.starvation_override, 1);
    V.params.starvation_override = 0;
    c = sel(0);
    CHECK_EQ(c.kind, FL_DK_EVENT);
    CHECK_EQ(c.reason, FL_R_EVENT_URGENT);
}

/* fresh with defer=0 must be indistinguishable from fresh_nodefer on every view. */
static void test_defer0_equals_nodefer(void)
{
    static const fl_time_t acked_opts[2] = {FL_TIME_NONE, 4000};
    static const fl_time_t tx_opts[2] = {FL_TIME_NONE, 100};
    int ne, late, ns, ak, ld, tx;
    int views = 0;
    for (ne = 0; ne < 3; ne++)
        for (late = 0; late < 2; late++)
            for (ns = 0; ns < 3; ns++)
                for (ak = 0; ak < 2; ak++)
                    for (ld = 0; ld < 2; ld++)
                        for (tx = 0; tx < 2; tx++) {
                            static const uint8_t nev[3] = {0, 1, 3};
                            static const uint8_t nst[3] = {0, 1, 3};
                            fl_choice_t a, b;
                            uint8_t ra = 0, rb = 0;
                            uint8_t i;
                            int k;
                            for (k = 0; k < 2; k++) {
                                vinit(k == 0 ? "fresh" : "fresh_nodefer", 5000);
                                V.params.defer = 0;
                                V.params.late_demote = (uint8_t)ld;
                                V.rr_next = 2;
                                for (i = 0; i < nev[ne]; i++) {
                                    /* late variant: deadlines below now; else above */
                                    vev((uint32_t)(i + 1u), late ? 4000u + 10u * i : 5100u + 200u * i, 4000u);
                                }
                                for (i = 0; i < nst[ns]; i++) {
                                    vst(i, 4900, tx_opts[tx], acked_opts[ak]);
                                }
                                if (k == 0) {
                                    a = sel(&ra);
                                } else {
                                    b = sel(&rb);
                                }
                            }
                            CHECK_EQ(a.kind, b.kind);
                            CHECK_EQ(a.reason, b.reason);
                            CHECK_EQ(a.stream, b.stream);
                            CHECK_EQ(a.event_id, b.event_id);
                            CHECK_EQ(a.n_elig_ev, b.n_elig_ev);
                            CHECK_EQ(a.n_elig_st, b.n_elig_st);
                            CHECK_EQ(ra, rb);
                            views++;
                        }
    CHECK_EQ(views, 144);
}

/* Late demotion is applied identically by every family. */
static void test_late_demotion_all_families(void)
{
    static const char *fam[4] = {"edf_rr", "fresh_nodefer", "fresh", "fifo"};
    static const uint8_t state_reason[4] = {FL_R_STATE_RR, FL_R_STATE_FRESH, FL_R_STATE_FRESH, FL_R_FIFO_OLDEST};
    int i;
    for (i = 0; i < 4; i++) {
        fl_choice_t c;
        /* off: the late event goes first */
        vinit(fam[i], 50);
        vev(1, 40, 0);
        vst(0, 20, 0, 0);
        c = sel(0);
        CHECK_EQ(c.kind, FL_DK_EVENT);
        CHECK_EQ(c.reason, FL_R_EVENT_LATE);
        /* on: the stream goes first with the family's own state reason */
        vinit(fam[i], 50);
        V.params.late_demote = 1;
        vev(1, 40, 0);
        vst(0, 20, 0, 0);
        c = sel(0);
        CHECK_EQ(c.kind, FL_DK_STATE);
        CHECK_EQ(c.stream, 0);
        CHECK_EQ(c.reason, state_reason[i]);
        /* on, no streams: the late event is served as EVENT_LATE_DEMOTED */
        vinit(fam[i], 50);
        V.params.late_demote = 1;
        vev(1, 40, 0);
        c = sel(0);
        CHECK_EQ(c.kind, FL_DK_EVENT);
        CHECK_EQ(c.event_id, 1);
        CHECK_EQ(c.reason, FL_R_EVENT_LATE_DEMOTED);
        /* two late events: EDF among them, sorted (30,id 2),(40,id 1) -> id 2 */
        vinit(fam[i], 50);
        V.params.late_demote = 1;
        vev(2, 30, 0);
        vev(1, 40, 0);
        c = sel(0);
        CHECK_EQ(c.event_id, 2);
        CHECK_EQ(c.reason, FL_R_EVENT_LATE_DEMOTED);
        /* on, one late and one fresh event: the fresh one is served normally first */
        vinit(fam[i], 50);
        V.params.late_demote = 1;
        vev(2, 30, 0);
        vev(1, 400, 10);
        c = sel(0);
        CHECK_EQ(c.kind, FL_DK_EVENT);
        CHECK_EQ(c.event_id, 1);
        CHECK(c.reason != FL_R_EVENT_LATE_DEMOTED);
    }
}

static void test_fifo(void)
{
    fl_choice_t c;
    /* event gen 30 vs stream latest 20 -> stream */
    vinit("fifo", 100);
    vev(1, 500, 30);
    vst(0, 20, 0, 0);
    c = sel(0);
    CHECK_EQ(c.kind, FL_DK_STATE);
    CHECK_EQ(c.stream, 0);
    CHECK_EQ(c.reason, FL_R_FIFO_OLDEST);
    /* equal gen -> event first */
    vinit("fifo", 100);
    vev(1, 500, 20);
    vst(0, 20, 0, 0);
    c = sel(0);
    CHECK_EQ(c.kind, FL_DK_EVENT);
    CHECK_EQ(c.reason, FL_R_FIFO_OLDEST);
    /* two streams -> smaller latest_gen */
    vinit("fifo", 100);
    vst(0, 20, 0, 0);
    vst(1, 15, 0, 0);
    c = sel(0);
    CHECK_EQ(c.stream, 1);
    /* two events same gen: lower id (view sorted by deadline puts id 2 first) */
    vinit("fifo", 100);
    vev(2, 50, 10);
    vev(1, 60, 10);
    c = sel(0);
    CHECK_EQ(c.event_id, 1);
    /* older gen wins over EDF order */
    vinit("fifo", 100);
    vev(2, 50, 10);
    vev(1, 60, 5);
    c = sel(0);
    CHECK_EQ(c.event_id, 1);
}

static void test_empty_and_args(void)
{
    static const char *fam[4] = {"edf_rr", "fresh_nodefer", "fresh", "fifo"};
    int i;
    fl_choice_t c;
    uint8_t rr;
    for (i = 0; i < 4; i++) {
        vinit(fam[i], 100);
        V.rr_next = 3;
        c = sel(&rr);
        CHECK_EQ(c.kind, FL_DK_NONE);
        CHECK_EQ(c.reason, FL_R_IDLE);
        CHECK_EQ(c.n_elig_ev, 0);
        CHECK_EQ(c.n_elig_st, 0);
        CHECK_EQ(rr, 3);
    }
    vinit("edf_rr", 100);
    V.n_events = (uint8_t)(FL_EVENT_CAPACITY + 1u);
    CHECK_EQI(fl_policy_select(&V, &c, &rr), FL_ERR_ARG);
    vinit("edf_rr", 100);
    CHECK_EQI(fl_policy_select(0, &c, &rr), FL_ERR_ARG);
    CHECK_EQI(fl_policy_select(&V, 0, &rr), FL_ERR_ARG);
    CHECK_EQI(fl_policy_select(&V, &c, 0), FL_ERR_ARG);
    V.params.family = 9;
    CHECK_EQI(fl_policy_select(&V, &c, &rr), FL_ERR_ARG);
}

static void test_presets(void)
{
    fl_policy_params_t p;
    CHECK_EQI(fl_policy_family_from_name("edf_rr", &p), FL_OK);
    CHECK(p.family == FL_POL_EDF_RR && p.defer == 0 && p.late_demote == 0 && p.starvation_override == 0);
    CHECK_EQI(fl_policy_family_from_name("edf_rr_ld", &p), FL_OK);
    CHECK(p.family == FL_POL_EDF_RR && p.late_demote == 1);
    CHECK_EQI(fl_policy_family_from_name("fresh_nodefer", &p), FL_OK);
    CHECK(p.family == FL_POL_FRESH_NODEFER && p.defer == 0 && p.late_demote == 0);
    CHECK_EQI(fl_policy_family_from_name("fresh_nodefer_ld", &p), FL_OK);
    CHECK(p.family == FL_POL_FRESH_NODEFER && p.late_demote == 1);
    CHECK_EQI(fl_policy_family_from_name("fresh", &p), FL_OK);
    CHECK(p.family == FL_POL_FRESH && p.defer == 1 && p.late_demote == 0 && p.starvation_override == 0);
    CHECK_EQI(fl_policy_family_from_name("fresh_ld", &p), FL_OK);
    CHECK(p.family == FL_POL_FRESH && p.defer == 1 && p.late_demote == 1);
    CHECK_EQI(fl_policy_family_from_name("fresh_so", &p), FL_OK);
    CHECK(p.family == FL_POL_FRESH && p.defer == 1 && p.starvation_override == 1 && p.late_demote == 0);
    CHECK_EQI(fl_policy_family_from_name("fifo", &p), FL_OK);
    CHECK(p.family == FL_POL_FIFO && p.late_demote == 0);
    CHECK_EQI(fl_policy_family_from_name("fifo_ld", &p), FL_OK);
    CHECK(p.family == FL_POL_FIFO && p.late_demote == 1);
    CHECK_EQI(fl_policy_family_from_name("nope", &p), FL_ERR_ARG);
    CHECK_EQI(fl_policy_family_from_name(0, &p), FL_ERR_ARG);
    CHECK(strcmp(fl_policy_family_name(FL_POL_FRESH), "fresh") == 0);
}

int main(void)
{
    RUN(test_edf_rr);
    RUN(test_fresh_nodefer);
    RUN(test_fresh_urgency_queue_term);
    RUN(test_starvation_ranking_regression);
    RUN(test_starvation_override);
    RUN(test_defer0_equals_nodefer);
    RUN(test_late_demotion_all_families);
    RUN(test_fifo);
    RUN(test_empty_and_args);
    RUN(test_presets);
    return FLTEST_REPORT("test_policy");
}
