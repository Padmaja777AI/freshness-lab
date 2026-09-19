/*
 * Freshness Lab — HOST SIMULATION harness library (docs/DESIGN.md §8).
 * Everything here may use the heap and stdio; nothing here is embedded code.
 * SPDX-License-Identifier: MIT
 */
#ifndef FL_HOST_SIM_H
#define FL_HOST_SIM_H

#include <stdint.h>
#include <stdio.h>
#include "../core/fl_sender.h"
#include "../core/fl_receiver.h"
#include "aoi.h"

enum wl_action { WL_STATE = 1, WL_RAISE = 2, WL_CLEAR = 3 };

typedef struct {
    fl_time_t time_ms;
    uint8_t action;      /* enum wl_action */
    uint8_t stream;      /* STATE */
    int32_t value;       /* STATE */
    uint8_t code;        /* RAISE/CLEAR */
    fl_time_t deadline_rel;
    fl_time_t retention_rel;
} wl_action_t;

typedef struct {
    wl_action_t *items;
    uint32_t n;
    uint32_t n_events; /* number of RAISE/CLEAR actions */
} workload_t;

typedef struct {
    uint32_t n_slots;
    uint8_t *data_lost;
    fl_time_t *data_delay;
    uint8_t *ack_lost;
    fl_time_t *ack_delay;
} trace_t;

typedef struct {
    fl_time_t run_ms;
    fl_time_t slot_ms;
    uint8_t n_streams;
    uint16_t session_id;
    fl_time_t ack_timeout_ms;
    uint8_t max_attempts;
    fl_time_t aoi_threshold_ms;
    fl_policy_params_t policy;
    char policy_name[32];
} sim_config_t;

typedef struct {
    uint32_t id;
    uint8_t kind;
    uint8_t code;
    fl_time_t gen_time;
    fl_time_t deadline_abs;
    fl_time_t retention_abs;
    uint8_t admitted;
    uint8_t outcome;         /* enum fl_terminal; PENDING = censored */
    fl_time_t terminal_time; /* FL_TIME_NONE if pending */
    uint8_t attempts;        /* updated on every transmission, not only at terminal */
    fl_time_t first_tx_time; /* FL_TIME_NONE if never transmitted */
    fl_time_t rx_first_time; /* FL_TIME_NONE if never delivered */
    uint8_t rx_on_time;
    uint32_t rx_dups;
    uint32_t rx_oow;         /* out-of-window rejections at the receiver */
} ev_record_t;

typedef struct {
    uint32_t slot;
    fl_time_t t;
    fl_choice_t choice;
    uint8_t attempt;
    uint32_t seq;
    uint8_t lost;
    fl_time_t delay;
    uint8_t ack_len;         /* reverse frame emitted this slot (0 = none) */
    uint8_t ack_type;
    uint8_t ack_lost;
} dec_record_t;

typedef struct {
    fl_time_t t;
    uint8_t dir;             /* 1 = data at receiver, 2 = ack at sender */
    uint8_t outcome;         /* fl_rx_outcome for data; 0/err for ack */
    int err;
    uint8_t stream;
    uint32_t seq;
    uint32_t id;
    fl_time_t gen_time;
    uint8_t on_time;
} rx_record_t;

typedef struct {
    uint32_t published;
    uint32_t sent;
    uint32_t applied;
    uint32_t stale_dropped;
} stream_counts_t;

typedef struct {
    fl_sender_stats_t s;
    fl_receiver_stats_t r;
    ev_record_t *events;
    uint32_t n_events;       /* events actually generated in the run (IDs 1..n) */
    uint32_t ledger_cap;     /* workload event rows (allocation size) */
    uint64_t ledger_attempt_sum; /* transmissions attributed to ledger records */
    uint32_t event_decisions;    /* EVENT decisions that produced a frame */
    uint8_t ledger_mismatch;     /* 1 if the reconciliation in sim_run failed */
    dec_record_t *decisions;
    uint32_t n_decisions;
    rx_record_t *rxlog;
    uint32_t n_rx;
    uint32_t rx_cap;
    aoi_acc_t aoi[FL_MAX_STREAMS];
    stream_counts_t sc[FL_MAX_STREAMS];
    uint32_t data_frames, ack_frames, data_lost, ack_lost, in_transit_end;
    uint32_t interval_violations;
    uint32_t ev_pending_end, delivered_but_unacked;
    uint64_t lat_sum;
    fl_time_t lat_max;
    uint32_t lat_n;
    uint64_t conf_sum;
    fl_time_t conf_max;
    uint32_t conf_n;
    int core_error;          /* first FL_ERR_* returned by an unexpected core call */
    char core_error_where[64];
} sim_result_t;

int sim_run(const sim_config_t *cfg, const workload_t *wl, const trace_t *tr, sim_result_t *out);
void sim_result_free(sim_result_t *r);

/* Writers (host files, docs/DESIGN.md §8.5). Return 0 on success. */
int sim_write_summary(FILE *fp, const sim_result_t *r, const sim_config_t *cfg, int header);
int sim_write_events(FILE *fp, const sim_result_t *r);
int sim_write_decisions(FILE *fp, const sim_result_t *r);
int sim_write_state(FILE *fp, const sim_result_t *r, const sim_config_t *cfg);
int sim_write_rxlog(FILE *fp, const sim_result_t *r);
int sim_write_config(FILE *fp, const sim_config_t *cfg);

/* CSV/config loaders (host). Return 0 on success, print reason to stderr. */
int load_workload(const char *path, workload_t *wl);
int load_trace(const char *path, trace_t *tr);
int load_config(const char *path, sim_config_t *cfg);
void workload_free(workload_t *wl);
void trace_free(trace_t *tr);
void sim_config_defaults(sim_config_t *cfg);

#endif
