/*
 * Freshness Lab — shared types, error codes, enums.
 * SPDX-License-Identifier: MIT
 */
#ifndef FL_TYPES_H
#define FL_TYPES_H

#include <stdint.h>
#include "fl_config.h"

typedef uint32_t fl_time_t;

enum fl_err {
    FL_OK = 0,
    FL_ERR_ARG = -1,
    FL_ERR_TIME_REGRESS = -2,
    FL_ERR_TIME_HORIZON = -3,
    FL_ERR_REL_RANGE = -4,
    FL_ERR_SEQ_EXHAUSTED = -5,
    FL_ERR_ID_EXHAUSTED = -6,
    FL_ERR_EVENT_FULL = -7,
    FL_ERR_FRAME_LEN = -8,
    FL_ERR_FRAME_MAGIC = -9,
    FL_ERR_FRAME_TYPE = -10,
    FL_ERR_FRAME_SESSION = -11,
    FL_ERR_FRAME_FIELD = -12,
    FL_ERR_ACK_IMPOSSIBLE = -13,
    FL_ERR_ACK_UNMATCHED = -14,
    FL_ERR_ACK_QUEUE_FULL = -15
};

enum fl_frame_type {
    FL_FT_STATE = 1,
    FL_FT_EVENT = 2,
    FL_FT_ACK_STATE = 3,
    FL_FT_ACK_EVENT = 4
};

enum fl_event_kind { FL_EV_RAISE = 1, FL_EV_CLEAR = 2 };

#define FL_ACKF_DUPLICATE 0x01u

/* Sender-side terminal outcomes, exactly one per generated event (§6.5). */
enum fl_terminal {
    FL_TERM_PENDING = 0,
    FL_TERM_REJECTED_FULL = 1,
    FL_TERM_ACKED = 2,
    FL_TERM_RETRY_EXHAUSTED = 3,
    FL_TERM_RETENTION_EXPIRED = 4
};

enum fl_decision_kind { FL_DK_NONE = 0, FL_DK_STATE = 1, FL_DK_EVENT = 2 };

enum fl_reason {
    FL_R_IDLE = 0,
    FL_R_EVENT_EDF = 1,
    FL_R_EVENT_LATE = 2,
    FL_R_EVENT_LATE_DEMOTED = 3,
    FL_R_EVENT_URGENT = 4,
    FL_R_EVENT_SLACK_OK = 5,
    FL_R_STATE_RR = 6,
    FL_R_STATE_FRESH = 7,
    FL_R_STATE_STARVATION = 8,
    FL_R_STATE_STALE = 9,
    FL_R_FIFO_OLDEST = 10,
    FL_R_COUNT = 11
};

enum fl_policy_family {
    FL_POL_EDF_RR = 0,
    FL_POL_FRESH_NODEFER = 1,
    FL_POL_FRESH = 2,
    FL_POL_FIFO = 3
};

typedef struct {
    uint8_t family;              /* enum fl_policy_family */
    uint8_t defer;               /* FRESH only: 1 = may defer an eligible event */
    uint8_t late_demote;         /* all: 1 = late events served only when idle */
    uint8_t starvation_override; /* FRESH only: 1 = guard outranks urgency (fresh_so) */
    fl_time_t event_service_ms;
    fl_time_t slack_guard_ms;
    fl_time_t state_stale_ms;
    fl_time_t state_starvation_ms;
} fl_policy_params_t;

typedef struct {
    uint8_t kind;      /* enum fl_decision_kind */
    uint8_t reason;    /* enum fl_reason */
    uint8_t stream;    /* valid when kind == STATE */
    uint32_t event_id; /* valid when kind == EVENT */
    uint8_t n_elig_ev;
    uint8_t n_elig_st;
} fl_choice_t;

const char *fl_reason_name(uint8_t reason);
const char *fl_terminal_name(uint8_t term);
const char *fl_err_name(int err);

#endif /* FL_TYPES_H */
