/* Freshness Lab — HOST AoI accumulator. SPDX-License-Identifier: MIT */
#include "aoi.h"

void aoi_init(aoi_acc_t *a, fl_time_t threshold_ms)
{
    a->defined = 0;
    a->first_apply = FL_TIME_NONE;
    a->seg_start = 0;
    a->cur_gen = 0;
    a->threshold = threshold_ms;
    a->area2 = 0;
    a->over_ms = 0;
    a->peak = 0;
    a->unknown_ms = 0;
    a->final_age = 0;
    a->finished = 0;
}

/* Close [seg_start, t): age a0 = seg_start - cur_gen rising to a0 + n. */
static void close_segment(aoi_acc_t *a, fl_time_t t)
{
    uint64_t n = (uint64_t)(t - a->seg_start);
    uint64_t a0 = (uint64_t)(a->seg_start - a->cur_gen);
    uint64_t end_age = a0 + n;
    a->area2 += 2u * n * a0 + n * n;
    if (end_age > a->peak) {
        a->peak = (fl_time_t)end_age;
    }
    if (a0 >= a->threshold) {
        a->over_ms += n;
    } else if (end_age > a->threshold) {
        a->over_ms += end_age - a->threshold;
    }
}

void aoi_apply(aoi_acc_t *a, fl_time_t t, fl_time_t gen)
{
    if (gen > t) {
        gen = t; /* cannot happen with an honest sender; clamp defensively */
    }
    if (!a->defined) {
        a->defined = 1;
        a->first_apply = t;
        a->unknown_ms = t;
        a->seg_start = t;
        a->cur_gen = gen;
        return;
    }
    if (t < a->seg_start) {
        return; /* time regress: ignore (harness guarantees monotone time) */
    }
    close_segment(a, t);
    a->seg_start = t;
    a->cur_gen = gen;
}

void aoi_finish(aoi_acc_t *a, fl_time_t end)
{
    if (a->finished) {
        return;
    }
    a->finished = 1;
    if (!a->defined) {
        a->unknown_ms = end;
        return;
    }
    if (end > a->seg_start) {
        close_segment(a, end);
        a->seg_start = end;
    }
    a->final_age = end - a->cur_gen;
}

double aoi_area(const aoi_acc_t *a)
{
    return (double)a->area2 / 2.0;
}

double aoi_mean(const aoi_acc_t *a, fl_time_t end)
{
    if (!a->defined || end <= a->first_apply) {
        return 0.0;
    }
    return aoi_area(a) / (double)(end - a->first_apply);
}
