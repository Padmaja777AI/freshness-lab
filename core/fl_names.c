/* Freshness Lab — enum-to-string tables (no I/O). SPDX-License-Identifier: MIT */
#include "fl_types.h"

const char *fl_reason_name(uint8_t reason)
{
    switch (reason) {
    case FL_R_IDLE: return "IDLE";
    case FL_R_EVENT_EDF: return "EVENT_EDF";
    case FL_R_EVENT_LATE: return "EVENT_LATE";
    case FL_R_EVENT_LATE_DEMOTED: return "EVENT_LATE_DEMOTED";
    case FL_R_EVENT_URGENT: return "EVENT_URGENT";
    case FL_R_EVENT_SLACK_OK: return "EVENT_SLACK_OK";
    case FL_R_STATE_RR: return "STATE_RR";
    case FL_R_STATE_FRESH: return "STATE_FRESH";
    case FL_R_STATE_STARVATION: return "STATE_STARVATION";
    case FL_R_STATE_STALE: return "STATE_STALE";
    case FL_R_FIFO_OLDEST: return "FIFO_OLDEST";
    default: return "?";
    }
}

const char *fl_terminal_name(uint8_t term)
{
    switch (term) {
    case FL_TERM_PENDING: return "PENDING";
    case FL_TERM_REJECTED_FULL: return "REJECTED_FULL";
    case FL_TERM_ACKED: return "ACKED";
    case FL_TERM_RETRY_EXHAUSTED: return "RETRY_EXHAUSTED";
    case FL_TERM_RETENTION_EXPIRED: return "RETENTION_EXPIRED";
    default: return "?";
    }
}

const char *fl_err_name(int err)
{
    switch (err) {
    case FL_OK: return "OK";
    case FL_ERR_ARG: return "ARG";
    case FL_ERR_TIME_REGRESS: return "TIME_REGRESS";
    case FL_ERR_TIME_HORIZON: return "TIME_HORIZON";
    case FL_ERR_REL_RANGE: return "REL_RANGE";
    case FL_ERR_SEQ_EXHAUSTED: return "SEQ_EXHAUSTED";
    case FL_ERR_ID_EXHAUSTED: return "ID_EXHAUSTED";
    case FL_ERR_EVENT_FULL: return "EVENT_FULL";
    case FL_ERR_FRAME_LEN: return "FRAME_LEN";
    case FL_ERR_FRAME_MAGIC: return "FRAME_MAGIC";
    case FL_ERR_FRAME_TYPE: return "FRAME_TYPE";
    case FL_ERR_FRAME_SESSION: return "FRAME_SESSION";
    case FL_ERR_FRAME_FIELD: return "FRAME_FIELD";
    case FL_ERR_ACK_IMPOSSIBLE: return "ACK_IMPOSSIBLE";
    case FL_ERR_ACK_UNMATCHED: return "ACK_UNMATCHED";
    case FL_ERR_ACK_QUEUE_FULL: return "ACK_QUEUE_FULL";
    default: return "?";
    }
}
