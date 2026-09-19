/* Freshness Lab — HOST SIMULATION command-line runner. SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "sim.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>

static void usage(void)
{
    fprintf(stderr,
            "usage: flsim --workload W.csv --trace T.csv --config C.cfg --policy NAME --out DIR\n"
            "             [--defer 0|1] [--late-demote 0|1] [--starvation-override 0|1] [--quiet]\n"
            "policies: edf_rr edf_rr_ld fresh_nodefer fresh_nodefer_ld fresh fresh_ld fresh_so fifo fifo_ld\n"
            "HOST SIMULATION only: no board, no real link.\n");
}

static int mkdir_p(const char *dir)
{
    char tmp[512];
    size_t n = strlen(dir);
    size_t i;
    if (n == 0 || n >= sizeof(tmp)) {
        return -1;
    }
    strcpy(tmp, dir);
    for (i = 1; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static FILE *open_out(const char *dir, const char *name)
{
    char path[640];
    FILE *fp;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    fp = fopen(path, "w");
    if (fp == 0) {
        fprintf(stderr, "flsim: cannot write %s\n", path);
    }
    return fp;
}

int main(int argc, char **argv)
{
    const char *wpath = 0, *tpath = 0, *cpath = 0, *pname = 0, *odir = 0;
    int defer = -1, late = -1, so = -1, quiet = 0;
    sim_config_t cfg;
    workload_t wl;
    trace_t tr;
    sim_result_t res;
    struct timespec t0, t1;
    FILE *fp;
    int i;
    int rc;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--workload") == 0 && i + 1 < argc) wpath = argv[++i];
        else if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc) tpath = argv[++i];
        else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) cpath = argv[++i];
        else if (strcmp(argv[i], "--policy") == 0 && i + 1 < argc) pname = argv[++i];
        else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) odir = argv[++i];
        else if (strcmp(argv[i], "--defer") == 0 && i + 1 < argc) defer = atoi(argv[++i]);
        else if (strcmp(argv[i], "--late-demote") == 0 && i + 1 < argc) late = atoi(argv[++i]);
        else if (strcmp(argv[i], "--starvation-override") == 0 && i + 1 < argc) so = atoi(argv[++i]);
        else if (strcmp(argv[i], "--quiet") == 0) quiet = 1;
        else {
            usage();
            return 2;
        }
    }
    if (wpath == 0 || tpath == 0 || cpath == 0 || pname == 0 || odir == 0) {
        usage();
        return 2;
    }
    sim_config_defaults(&cfg);
    if (load_config(cpath, &cfg) != 0) {
        return 1;
    }
    if (fl_policy_family_from_name(pname, &cfg.policy) != FL_OK) {
        fprintf(stderr, "flsim: unknown policy %s\n", pname);
        usage();
        return 2;
    }
    strncpy(cfg.policy_name, pname, sizeof(cfg.policy_name) - 1u);
    cfg.policy_name[sizeof(cfg.policy_name) - 1u] = '\0';
    if (defer >= 0) cfg.policy.defer = (uint8_t)(defer ? 1 : 0);
    if (late >= 0) cfg.policy.late_demote = (uint8_t)(late ? 1 : 0);
    if (so >= 0) cfg.policy.starvation_override = (uint8_t)(so ? 1 : 0);
    if (load_workload(wpath, &wl) != 0) {
        return 1;
    }
    if (load_trace(tpath, &tr) != 0) {
        workload_free(&wl);
        return 1;
    }
    if (mkdir_p(odir) != 0) {
        fprintf(stderr, "flsim: cannot create %s\n", odir);
        workload_free(&wl);
        trace_free(&tr);
        return 1;
    }
    clock_gettime(CLOCK_MONOTONIC, &t0);
    rc = sim_run(&cfg, &wl, &tr, &res);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (rc != 0) {
        fprintf(stderr, "flsim: simulation failed\n");
        sim_result_free(&res);
        workload_free(&wl);
        trace_free(&tr);
        return 1;
    }
    if ((fp = open_out(odir, "summary.csv")) == 0) return 1;
    sim_write_summary(fp, &res, &cfg, 1);
    fclose(fp);
    if ((fp = open_out(odir, "events.csv")) == 0) return 1;
    sim_write_events(fp, &res);
    fclose(fp);
    if ((fp = open_out(odir, "decisions.csv")) == 0) return 1;
    sim_write_decisions(fp, &res);
    fclose(fp);
    if ((fp = open_out(odir, "state.csv")) == 0) return 1;
    sim_write_state(fp, &res, &cfg);
    fclose(fp);
    if ((fp = open_out(odir, "rx.csv")) == 0) return 1;
    sim_write_rxlog(fp, &res);
    fclose(fp);
    if ((fp = open_out(odir, "config.txt")) == 0) return 1;
    sim_write_config(fp, &cfg);
    fclose(fp);
    if ((fp = open_out(odir, "runtime.txt")) == 0) return 1;
    {
        double us = (double)(t1.tv_sec - t0.tv_sec) * 1e6 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e3;
        fprintf(fp, "# HOST runtime only (desktop CPU); not an embedded measurement\nhost_runtime_us=%.0f\n", us);
    }
    fclose(fp);
    if (!quiet) {
        printf("HOST SIMULATION %s: gen=%u adm=%u rej=%u acked=%u exh=%u exp=%u pend=%u | rx del=%u ontime=%u "
               "late=%u dup=%u oow=%u | aoi_mean_s0=%.1f | bytes=%llu | interval_viol=%u core_err=%d\n",
               cfg.policy_name, res.s.events_generated, res.s.events_admitted, res.s.events_rejected_full,
               res.s.events_acked, res.s.events_retry_exhausted, res.s.events_retention_expired, res.ev_pending_end,
               res.r.ev_delivered, res.r.ev_on_time, res.r.ev_late, res.r.ev_duplicate, res.r.ev_out_of_window,
               aoi_mean(&res.aoi[0], cfg.run_ms),
               (unsigned long long)(res.s.bytes_data_tx + res.r.bytes_ack_tx), res.interval_violations,
               res.core_error);
    }
    if (res.ledger_mismatch) {
        fprintf(stderr, "flsim: ledger reconciliation failed (attempts vs event_tx vs decisions)\n");
    }
    rc = (res.core_error != 0 || res.ledger_mismatch) ? 3 : 0;
    sim_result_free(&res);
    workload_free(&wl);
    trace_free(&tr);
    return rc;
}
