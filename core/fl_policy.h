/*
 * Freshness Lab — scheduling policies (docs/DESIGN.md §9).
 *
 * Structural separation: this module sees ONLY fl_sched_view_t, a read-only
 * snapshot the sender builds from its own ledger and last-ACKed metadata. It
 * has no access to the receiver, the channel trace, the workload, or future
 * time. It must not include any receiver or host header.
 * SPDX-License-Identifier: MIT
 */
#ifndef FL_POLICY_H
#define FL_POLICY_H

#include "fl_types.h"

typedef struct {
    uint32_t id;
    fl_time_t deadline_abs;
    fl_time_t gen_time;
} fl_view_event_t;

typedef struct {
    uint8_t stream;
    fl_time_t latest_gen;     /* generation time of the waiting snapshot   */
    fl_time_t last_tx_time;   /* FL_TIME_NONE until the stream's first tx  */
    fl_time_t last_acked_gen; /* FL_TIME_NONE until the first ACK_STATE    */
} fl_view_stream_t;

typedef struct {
    fl_time_t now;
    fl_policy_params_t params;
    uint8_t n_events;                              /* eligible events        */
    fl_view_event_t events[FL_EVENT_CAPACITY];     /* sorted (deadline, id)  */
    uint8_t n_streams;                             /* eligible streams       */
    fl_view_stream_t streams[FL_MAX_STREAMS];      /* ascending stream id    */
    uint8_t n_streams_total;                       /* configured stream count*/
    uint8_t rr_next;                               /* round-robin cursor     */
} fl_sched_view_t;

/*
 * Chooses at most one item. *rr_next_out receives the (possibly updated)
 * round-robin cursor. Returns FL_OK or FL_ERR_ARG. Pure function of *view.
 */
int fl_policy_select(const fl_sched_view_t *view, fl_choice_t *out, uint8_t *rr_next_out);

/* Helpers exposed for tests. */
uint8_t fl_policy_event_is_late(const fl_sched_view_t *view, const fl_view_event_t *e);
int fl_policy_family_from_name(const char *name, fl_policy_params_t *p);
const char *fl_policy_family_name(uint8_t family);

#endif /* FL_POLICY_H */
