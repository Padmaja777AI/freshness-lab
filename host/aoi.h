/*
 * Freshness Lab — HOST metric: continuous-time Age of Information accumulator
 * (docs/DESIGN.md §8.6). Host-only; not part of the embedded core.
 * SPDX-License-Identifier: MIT
 */
#ifndef FL_HOST_AOI_H
#define FL_HOST_AOI_H

#include <stdint.h>
#include "../core/fl_types.h"

typedef struct {
    uint8_t defined;       /* 1 after the first apply */
    fl_time_t first_apply; /* time of the first apply */
    fl_time_t seg_start;   /* start of the open segment */
    fl_time_t cur_gen;     /* generation time of the current snapshot */
    fl_time_t threshold;   /* aoi_threshold_ms */
    uint64_t area2;        /* 2 * continuous-time area under AoI(t) */
    uint64_t over_ms;      /* measure of {t : AoI(t) > threshold} */
    fl_time_t peak;        /* largest age reached at the end of any segment */
    fl_time_t unknown_ms;  /* time before the first apply */
    fl_time_t final_age;   /* age at the end of the run */
    uint8_t finished;
} aoi_acc_t;

void aoi_init(aoi_acc_t *a, fl_time_t threshold_ms);
/* Snapshot with generation time gen applied at time t (t >= previous t, gen <= t). */
void aoi_apply(aoi_acc_t *a, fl_time_t t, fl_time_t gen);
/* Close the last segment at end (run_ms). */
void aoi_finish(aoi_acc_t *a, fl_time_t end);
/* area / (end - first_apply); 0 if never defined. */
double aoi_mean(const aoi_acc_t *a, fl_time_t end);
/* area as a double (area2 / 2). */
double aoi_area(const aoi_acc_t *a);

#endif
