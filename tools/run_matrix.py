#!/usr/bin/env python3
"""
Freshness Lab -- HOST SIMULATION matrix runner (docs/DESIGN.md S9.3, S9.5, S10, S11).

    python3 tools/run_matrix.py --flsim build/flsim --out results/matrix \
        [--seeds 101 102 103 104 105] [--scenarios NAME ...] \
        [--policies edf_rr edf_rr_ld fresh_nodefer fresh fresh_ld fresh_so fifo] \
        [--keep-decisions alarm_outage,tight_deadline]

Pipeline
  0. Ledger regression fixtures (preflight). Tiny hand-computed flsim runs that
     must hold before any matrix result is accepted: no phantom PENDING ledger
     rows for workload rows outside the run, per-event `attempts` updated on
     every transmission (also for events still pending at cutoff), and the
     per-event attempt sum reconciled with the sender counter and the EVENT
     decisions. Written to OUT/checks/ledger_fixtures/.
  1. For each scenario x seed: generate workload/trace rows with
     tools/gen_workload.py / tools/gen_trace.py, write them to
     OUT/inputs/<scenario>/seed<seed>/, copy scenarios/<scenario>.cfg.
  2. For each policy: run flsim into OUT/runs/<scenario>/seed<seed>/<policy>/
     (a non-zero exit aborts the matrix, loudly).
  3. Ablation checks (S9.3 / S9.2): `fresh --defer 0` must produce a
     byte-identical decisions.csv to `fresh_nodefer`; `edf_rr` and
     `fresh_nodefer` must agree (kind, reason, ref) on every slot with eligible
     events. Plus per run: interval_violations == 0 (S6.4) and the accounting
     identity (S8.5). All recorded in OUT/checks.json.
  4. decisions.csv and rx.csv are deleted from run directories except for the
     --keep-decisions scenarios.
  5. Aggregation (tools/summarize.py), chart (tools/plot_svg.py), manifest.

Exit status: 0 all good; 1 a recorded check failed (outputs still written);
2 setup, generator or flsim failure (aborts).

HOST SIMULATION only: nothing here is an energy, hardware or superiority
measurement. Standard library only.
"""
from __future__ import annotations

import argparse
import importlib
import os
import shutil
import subprocess
import sys

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_DIR = os.path.dirname(TOOLS_DIR)
sys.path.insert(0, TOOLS_DIR)

import summarize  # noqa: E402  (tools/summarize.py)

DEFAULT_SEEDS = [101, 102, 103, 104, 105]
DEFAULT_SCENARIOS = [
    "healthy_light",
    "alarm_outage",
    "burst_loss",
    "ack_loss",
    "reorder",
    "overflow",
    "overload",
    "tight_deadline",
]
DEFAULT_POLICIES = ["edf_rr", "edf_rr_ld", "fresh_nodefer", "fresh", "fresh_ld", "fresh_so", "fifo"]
DEFAULT_KEEP = "alarm_outage,tight_deadline"
KNOWN_POLICIES = set(summarize.POLICY_ORDER)
# Policies the ablation checks need; run even when not requested (never aggregated unless requested).
CHECK_POLICIES = ["edf_rr", "fresh_nodefer", "fresh"]

WORKLOAD_HEADER = "time_ms,action,a,b,c"
TRACE_HEADER = "slot,data_lost,data_delay_ms,ack_lost,ack_delay_ms"
PRUNE_FILES = ("decisions.csv", "rx.csv")


def fail(msg, code=2):
    print(f"run_matrix: FAIL: {msg}", file=sys.stderr)
    sys.exit(code)


# --------------------------------------------------------------------------
# Generators (imported from tools/ at runtime)
# --------------------------------------------------------------------------
def load_generators(gen_dir):
    """Import gen_workload / gen_trace from gen_dir (default: tools/)."""
    gen_dir = os.path.abspath(gen_dir)
    if gen_dir != TOOLS_DIR:
        sys.path.insert(0, gen_dir)
    try:
        gw = importlib.import_module("gen_workload")
        gt = importlib.import_module("gen_trace")
    except ImportError as exc:
        fail(f"cannot import generators from {gen_dir}: {exc} (tools/gen_workload.py and tools/gen_trace.py needed)")
    for mod, fn in ((gw, "generate_workload"), (gt, "generate_trace")):
        if not hasattr(mod, fn):
            fail(f"{mod.__file__}: missing {fn}(scenario, seed)")
    return gw, gt


def _cell(v):
    if v is None:
        return ""
    if isinstance(v, bool):
        return "1" if v else "0"
    return str(v)


def rows_to_lines(rows, header):
    """Normalise generator output (dicts, sequences or CSV strings) to CSV lines.

    A generator-supplied header row (first cell equal to the first column
    name) is dropped so the file carries exactly one header.
    """
    cols = header.split(",")
    lines = [header]
    for r in rows:
        if isinstance(r, str):
            line = r.strip()
            if not line or line.startswith("#") or line.startswith(cols[0] + ","):
                continue
            lines.append(line)
        elif isinstance(r, dict):
            if all(c in r for c in cols):
                vals = [r[c] for c in cols]
            else:
                vals = list(r.values())
            lines.append(",".join(_cell(v) for v in vals))
        else:
            vals = list(r)
            if vals and isinstance(vals[0], str) and vals[0] == cols[0]:
                continue
            lines.append(",".join(_cell(v) for v in vals))
    return lines


def write_rows_csv(path, rows, header):
    lines = rows_to_lines(rows, header)
    with open(path, "w", encoding="utf-8", newline="\n") as fp:
        fp.write("\n".join(lines) + "\n")
    return len(lines) - 1


def write_text(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as fp:
        fp.write(text)


# --------------------------------------------------------------------------
# flsim
# --------------------------------------------------------------------------
def run_flsim(flsim, workload, trace, config, policy, out_dir, extra=(), check=True):
    cmd = [
        flsim,
        "--workload",
        workload,
        "--trace",
        trace,
        "--config",
        config,
        "--policy",
        policy,
        "--out",
        out_dir,
        "--quiet",
        *extra,
    ]
    os.makedirs(out_dir, exist_ok=True)
    proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if check and proc.returncode != 0:
        fail(
            f"flsim exited {proc.returncode} (3 = core_error/ledger_mismatch, 1 = load error)\n"
            f"  command: {' '.join(cmd)}\n  stdout: {proc.stdout.strip()}\n  stderr: {proc.stderr.strip()}"
        )
    return proc


def read_summary(run_dir):
    _, rows = summarize.read_csv(os.path.join(run_dir, "summary.csv"))
    if len(rows) != 1:
        fail(f"{run_dir}/summary.csv: expected one row, got {len(rows)}")
    return rows[0]


# --------------------------------------------------------------------------
# Ledger regression fixtures (user-requested; hand-computed from DESIGN.md)
# --------------------------------------------------------------------------
def _fixture_result(name, policy, expected, observed, note):
    mism = {}
    for k, v in expected.items():
        if str(v) != str(observed.get(k)):
            mism[k] = {"expected": str(v), "observed": None if observed.get(k) is None else str(observed.get(k))}
    return {
        "fixture": name,
        "policy": policy,
        "passed": not mism,
        "note": note,
        "expected": {k: str(v) for k, v in expected.items()},
        "observed": {k: (None if v is None else str(v)) for k, v in observed.items()},
        "mismatches": mism,
    }


def _events_rows(run_dir):
    return summarize.read_csv(os.path.join(run_dir, "events.csv"))[1]


def _decision_rows(run_dir):
    return summarize.read_csv(os.path.join(run_dir, "decisions.csv"))[1]


def ledger_preflight(flsim, fx_dir, policies):
    """Run the ledger regression fixtures; returns a list of check dicts.

    Hand computations (DESIGN.md):
    F1 out_of_run_row_rejected: run_ms=10, slot_ms=10, sole RAISE at t=10.
       S8.1 runs t over 0..run_ms-1, so the row can never execute; the
       harness must reject the workload (exit 1, no summary.csv) instead of
       reporting a zero-initialised ledger slot as ev_pending_end=1.
    F2 no_events_no_phantom_rows: same config, one STATE row and no events:
       ev_generated=0 and therefore (S8.5 identity) ev_pending_end=0 and
       events.csv has no data rows.
    F3 late_row_never_transmitted: RAISE at t=9 (inside the run). The only
       slot is t=0 (t % 10 == 0), the event is posted at t=9, no later slot
       exists: ev_generated=1, admitted=1, event_tx=0, PENDING with
       attempts=0 and first_tx_time '-' (S8.7 censoring).
    F4 pending_attempt_counted_at_cutoff (every policy): run_ms=1, slot_ms=1,
       RAISE at t=0 with deadline_rel=5, retention_rel=10, data lost at slot 0.
       Slot 0 sends attempt 1 (S6.3: attempts++ on sending), the frame is
       lost, the run ends: PENDING with attempts=1, first_tx_time=0,
       event_tx=1, event_retries=0, data_frames=1, bytes_data_tx=27 (S5:
       EVENT frame 27 bytes, charged although lost, S8.3), ack_frames=0,
       rx_ev_delivered=0, ledger_mismatch=0; decisions.csv has exactly one
       row: slot 0, kind EVENT, ref 1, attempt 1, n_elig_ev 1, lost 1;
       reason EVENT_EDF for the EDF-ordered families (deadline_abs=5 >= now=0
       so not late; S is empty so fresh takes step 2) and FIFO_OLDEST for
       fifo (S9.4). Reconciliation: sum(attempts) == event_tx == number of
       EVENT decisions == 1.
    F5 retries_reconcile_at_cutoff (every policy): run_ms=3, slot_ms=1,
       ack_timeout_ms=1, max_attempts=3, same event, all three data slots
       lost. t=0 attempt 1; t=1 housekeeping: 1-0 >= 1 so the slot leaves
       flight, attempts 1 < 3 so it is eligible again and attempt 2 goes out;
       t=2 likewise attempt 3. The exhaustion verdict would only be reached
       at the next timeout step (t=3), which never runs: PENDING with
       attempts=3, event_tx=3, event_retries=2, bytes_data_tx=81,
       ev_retry_exhausted=0; decisions attempts 1,2,3 on slots 0,1,2.
    """
    os.makedirs(fx_dir, exist_ok=True)
    inp = os.path.join(fx_dir, "inputs")
    os.makedirs(inp, exist_ok=True)
    cfg10 = os.path.join(inp, "run10_slot10.cfg")
    cfg1 = os.path.join(inp, "run1_slot1.cfg")
    cfg3 = os.path.join(inp, "run3_slot1_to1_max3.cfg")
    write_text(cfg10, "# ledger fixture\nrun_ms=10\nslot_ms=10\nn_streams=1\n")
    write_text(cfg1, "# ledger fixture\nrun_ms=1\nslot_ms=1\nn_streams=1\n")
    write_text(cfg3, "# ledger fixture\nrun_ms=3\nslot_ms=1\nn_streams=1\nack_timeout_ms=1\nmax_attempts=3\n")
    w_out = os.path.join(inp, "wl_event_at_10.csv")
    w_noev = os.path.join(inp, "wl_state_only.csv")
    w_at9 = os.path.join(inp, "wl_event_at_9.csv")
    w_at0 = os.path.join(inp, "wl_event_at_0.csv")
    write_text(w_out, WORKLOAD_HEADER + "\n10,RAISE,1,5,10\n")
    write_text(w_noev, WORKLOAD_HEADER + "\n0,STATE,0,7,\n")
    write_text(w_at9, WORKLOAD_HEADER + "\n9,RAISE,1,5,10\n")
    write_text(w_at0, WORKLOAD_HEADER + "\n0,RAISE,1,5,10\n")
    t_ok1 = os.path.join(inp, "tr_1slot_ok.csv")
    t_lost1 = os.path.join(inp, "tr_1slot_data_lost.csv")
    t_lost3 = os.path.join(inp, "tr_3slots_data_lost.csv")
    write_text(t_ok1, TRACE_HEADER + "\n0,0,1,0,1\n")
    write_text(t_lost1, TRACE_HEADER + "\n0,1,1,0,1\n")
    write_text(t_lost3, TRACE_HEADER + "\n0,1,1,0,1\n1,1,1,0,1\n2,1,1,0,1\n")

    results = []

    # F1
    d = os.path.join(fx_dir, "F1_out_of_run_row_rejected", "edf_rr")
    proc = run_flsim(flsim, w_out, t_ok1, cfg10, "edf_rr", d, check=False)
    results.append(
        _fixture_result(
            "F1_out_of_run_row_rejected",
            "edf_rr",
            {"exit": 1, "summary_written": False},
            {
                "exit": proc.returncode,
                "summary_written": os.path.isfile(os.path.join(d, "summary.csv")),
                "stderr": proc.stderr.strip()[-200:],
            },
            "workload row at t=run_ms can never execute (S8.1); must be rejected, not a phantom PENDING row",
        )
    )

    # F2
    d = os.path.join(fx_dir, "F2_no_events_no_phantom_rows", "edf_rr")
    proc = run_flsim(flsim, w_noev, t_ok1, cfg10, "edf_rr", d, check=False)
    obs = {"exit": proc.returncode}
    if proc.returncode == 0:
        s = read_summary(d)
        obs.update({k: s[k] for k in ("ev_generated", "ev_pending_end", "ledger_mismatch", "core_error")})
        obs["events_rows"] = len(_events_rows(d))
    results.append(
        _fixture_result(
            "F2_no_events_no_phantom_rows",
            "edf_rr",
            {"exit": 0, "ev_generated": 0, "ev_pending_end": 0, "ledger_mismatch": 0, "core_error": 0, "events_rows": 0},
            obs,
            "no event generated -> no ledger record, ev_pending_end=0 (S8.5 identity with generated=0)",
        )
    )

    # F3
    d = os.path.join(fx_dir, "F3_late_row_never_transmitted", "edf_rr")
    proc = run_flsim(flsim, w_at9, t_ok1, cfg10, "edf_rr", d, check=False)
    obs = {"exit": proc.returncode}
    if proc.returncode == 0:
        s = read_summary(d)
        obs.update({k: s[k] for k in ("ev_generated", "ev_admitted", "ev_pending_end", "event_tx", "ledger_mismatch")})
        ev = _events_rows(d)
        obs["events_rows"] = len(ev)
        if len(ev) == 1:
            obs.update({k: ev[0][k] for k in ("id", "outcome", "attempts", "first_tx_time")})
    results.append(
        _fixture_result(
            "F3_late_row_never_transmitted",
            "edf_rr",
            {
                "exit": 0,
                "ev_generated": 1,
                "ev_admitted": 1,
                "ev_pending_end": 1,
                "event_tx": 0,
                "ledger_mismatch": 0,
                "events_rows": 1,
                "id": 1,
                "outcome": "PENDING",
                "attempts": 0,
                "first_tx_time": "-",
            },
            obs,
            "event posted at t=9 after the only slot (t=0): generated, admitted, never sent, censored PENDING",
        )
    )

    # F4 and F5 for every requested policy (plus the check policies)
    pols = list(dict.fromkeys(list(policies) + CHECK_POLICIES))
    for pol in pols:
        reason = "FIFO_OLDEST" if pol.startswith("fifo") else "EVENT_EDF"
        d = os.path.join(fx_dir, "F4_pending_attempt_counted_at_cutoff", pol)
        proc = run_flsim(flsim, w_at0, t_lost1, cfg1, pol, d, check=False)
        obs = {"exit": proc.returncode}
        if proc.returncode == 0:
            s = read_summary(d)
            obs.update(
                {
                    k: s[k]
                    for k in (
                        "ev_generated",
                        "ev_admitted",
                        "ev_pending_end",
                        "ev_acked",
                        "event_tx",
                        "event_retries",
                        "data_frames",
                        "bytes_data_tx",
                        "ack_frames",
                        "bytes_ack_tx",
                        "bytes_total_tx",
                        "rx_ev_delivered",
                        "ledger_mismatch",
                        "core_error",
                    )
                }
            )
            ev = _events_rows(d)
            dec = _decision_rows(d)
            obs["events_rows"] = len(ev)
            obs["decision_rows"] = len(dec)
            if len(ev) == 1:
                obs.update({k: ev[0][k] for k in ("outcome", "terminal_time", "attempts", "first_tx_time")})
            if len(dec) == 1:
                obs.update({"dec_" + k: dec[0][k] for k in ("slot", "t", "kind", "reason", "ref", "attempt", "n_elig_ev", "lost")})
            obs["attempts_sum"] = sum(int(e["attempts"]) for e in ev)
            obs["event_decisions"] = sum(1 for x in dec if x["kind"] == "EVENT")
        results.append(
            _fixture_result(
                "F4_pending_attempt_counted_at_cutoff",
                pol,
                {
                    "exit": 0,
                    "ev_generated": 1,
                    "ev_admitted": 1,
                    "ev_pending_end": 1,
                    "ev_acked": 0,
                    "event_tx": 1,
                    "event_retries": 0,
                    "data_frames": 1,
                    "bytes_data_tx": 27,
                    "ack_frames": 0,
                    "bytes_ack_tx": 0,
                    "bytes_total_tx": 27,
                    "rx_ev_delivered": 0,
                    "ledger_mismatch": 0,
                    "core_error": 0,
                    "events_rows": 1,
                    "decision_rows": 1,
                    "outcome": "PENDING",
                    "terminal_time": "-",
                    "attempts": 1,
                    "first_tx_time": 0,
                    "dec_slot": 0,
                    "dec_t": 0,
                    "dec_kind": "EVENT",
                    "dec_reason": reason,
                    "dec_ref": 1,
                    "dec_attempt": 1,
                    "dec_n_elig_ev": 1,
                    "dec_lost": 1,
                    "attempts_sum": 1,
                    "event_decisions": 1,
                },
                obs,
                "1 ms run: event sent (lost) at t=0, still pending at cutoff -> attempts=1 must be in events.csv",
            )
        )

        d = os.path.join(fx_dir, "F5_retries_reconcile_at_cutoff", pol)
        proc = run_flsim(flsim, w_at0, t_lost3, cfg3, pol, d, check=False)
        obs = {"exit": proc.returncode}
        if proc.returncode == 0:
            s = read_summary(d)
            obs.update(
                {
                    k: s[k]
                    for k in (
                        "ev_generated",
                        "ev_pending_end",
                        "ev_retry_exhausted",
                        "event_tx",
                        "event_retries",
                        "data_frames",
                        "bytes_data_tx",
                        "ledger_mismatch",
                        "core_error",
                    )
                }
            )
            ev = _events_rows(d)
            dec = _decision_rows(d)
            obs["events_rows"] = len(ev)
            if len(ev) == 1:
                obs.update({k: ev[0][k] for k in ("outcome", "attempts", "first_tx_time")})
            obs["dec_attempts"] = ";".join(x["attempt"] for x in dec if x["kind"] == "EVENT")
            obs["attempts_sum"] = sum(int(e["attempts"]) for e in ev)
            obs["event_decisions"] = sum(1 for x in dec if x["kind"] == "EVENT")
        results.append(
            _fixture_result(
                "F5_retries_reconcile_at_cutoff",
                pol,
                {
                    "exit": 0,
                    "ev_generated": 1,
                    "ev_pending_end": 1,
                    "ev_retry_exhausted": 0,
                    "event_tx": 3,
                    "event_retries": 2,
                    "data_frames": 3,
                    "bytes_data_tx": 81,
                    "ledger_mismatch": 0,
                    "core_error": 0,
                    "events_rows": 1,
                    "outcome": "PENDING",
                    "attempts": 3,
                    "first_tx_time": 0,
                    "dec_attempts": "1;2;3",
                    "attempts_sum": 3,
                    "event_decisions": 3,
                },
                obs,
                "three lost attempts (ack_timeout 1 ms) then cutoff: attempts=3 == event_tx == EVENT decisions",
            )
        )
    return results


# --------------------------------------------------------------------------
# Matrix checks
# --------------------------------------------------------------------------
def compare_bytes(path_a, path_b):
    with open(path_a, "rb") as fa, open(path_b, "rb") as fb:
        a, b = fa.read(), fb.read()
    if a == b:
        return True, f"identical ({len(a)} bytes)"
    la, lb = a.split(b"\n"), b.split(b"\n")
    for i, (x, y) in enumerate(zip(la, lb), start=1):
        if x != y:
            return False, f"first difference at line {i}: {x[:120]!r} vs {y[:120]!r}"
    return False, f"length differs: {len(a)} vs {len(b)} bytes"


def compare_event_slots(dec_a, dec_b):
    """S9.2 note: (kind, reason, ref) must agree on every slot where n_elig_ev > 0.

    The event subsystem (admission, timeouts, event ACKs, which the receiver
    always serves before state ACKs) does not depend on the state ranking, so
    n_elig_ev is expected to agree on every slot as well; a difference there is
    reported separately (n_elig_ev_mismatches) and also fails the check, since
    it would mean the two runs diverged on the event side.
    """
    _, ra = summarize.read_csv(dec_a)
    _, rb = summarize.read_csv(dec_b)
    res = {
        "slots_total": len(ra),
        "slots_compared": 0,
        "mismatches": 0,
        "n_elig_ev_mismatches": 0,
        "first_mismatch": None,
        "passed": False,
    }
    if len(ra) != len(rb):
        res["first_mismatch"] = f"row count differs: {len(ra)} vs {len(rb)}"
        return res
    for x, y in zip(ra, rb):
        if x["slot"] != y["slot"]:
            res["mismatches"] += 1
            if res["first_mismatch"] is None:
                res["first_mismatch"] = f"slot ids differ: {x['slot']} vs {y['slot']}"
            continue
        if x["n_elig_ev"] != y["n_elig_ev"]:
            res["n_elig_ev_mismatches"] += 1
            if res["first_mismatch"] is None:
                res["first_mismatch"] = (
                    f"slot {x['slot']} (t={x['t']}): n_elig_ev edf_rr={x['n_elig_ev']} vs "
                    f"fresh_nodefer={y['n_elig_ev']}"
                )
        if int(x["n_elig_ev"]) > 0 or int(y["n_elig_ev"]) > 0:
            res["slots_compared"] += 1
            ka = (x["kind"], x["reason"], x["ref"])
            kb = (y["kind"], y["reason"], y["ref"])
            if ka != kb:
                res["mismatches"] += 1
                if res["first_mismatch"] is None:
                    res["first_mismatch"] = f"slot {x['slot']} (t={x['t']}): edf_rr {ka} vs fresh_nodefer {kb}"
    res["passed"] = res["mismatches"] == 0 and res["n_elig_ev_mismatches"] == 0
    return res


ABLATION_SUMMARY_IGNORE = {"policy", "family", "defer"}


def compare_summaries(sum_a, sum_b):
    """Identical decisions imply identical summaries except the naming columns."""
    a = read_summary(sum_a)
    b = read_summary(sum_b)
    diffs = {k: [a[k], b.get(k)] for k in a if k not in ABLATION_SUMMARY_IGNORE and a[k] != b.get(k)}
    return not diffs, diffs


def accounting_identity(s):
    """S8.5: generated = rejected_full + acked + retry_exhausted + retention_expired + pending_end."""
    lhs = int(s["ev_generated"])
    rhs = (
        int(s["ev_rejected_full"])
        + int(s["ev_acked"])
        + int(s["ev_retry_exhausted"])
        + int(s["ev_retention_expired"])
        + int(s["ev_pending_end"])
    )
    return lhs == rhs, f"generated {lhs} vs sum of outcomes {rhs}"


def prune_run_dirs(runs_dir, keep_scenarios):
    removed = 0
    if not os.path.isdir(runs_dir):
        return 0
    for sc in sorted(os.listdir(runs_dir)):
        if sc in keep_scenarios:
            continue
        for root, _dirs, files in os.walk(os.path.join(runs_dir, sc)):
            for f in files:
                if f in PRUNE_FILES:
                    os.remove(os.path.join(root, f))
                    removed += 1
    return removed


def parse_keep(spec, scenarios):
    spec = (spec or "").strip()
    if spec.lower() in ("", "none"):
        return set()
    if spec.lower() == "all":
        return set(scenarios)
    return {s.strip() for s in spec.split(",") if s.strip()}


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(description="Freshness Lab HOST SIMULATION matrix runner.")
    ap.add_argument("--flsim", required=True, help="path to the flsim binary (build/flsim)")
    ap.add_argument("--out", required=True, help="output directory (e.g. results/matrix)")
    ap.add_argument("--seeds", nargs="+", type=int, default=DEFAULT_SEEDS)
    ap.add_argument("--scenarios", nargs="+", default=DEFAULT_SCENARIOS)
    ap.add_argument("--policies", nargs="+", default=DEFAULT_POLICIES)
    ap.add_argument(
        "--keep-decisions",
        default=DEFAULT_KEEP,
        help="comma-separated scenarios whose decisions.csv/rx.csv are kept ('all' or 'none')",
    )
    ap.add_argument("--scenarios-dir", default=os.path.join(REPO_DIR, "scenarios"), help="directory of <name>.cfg")
    ap.add_argument("--gen-dir", default=TOOLS_DIR, help="directory holding gen_workload.py/gen_trace.py (tests only)")
    args = ap.parse_args(argv)

    flsim = os.path.abspath(args.flsim)
    if not (os.path.isfile(flsim) and os.access(flsim, os.X_OK)):
        fail(f"{flsim}: flsim binary missing or not executable (build it with 'make BUILD=build-tools build-tools/flsim')")
    out_dir = os.path.abspath(args.out)
    os.makedirs(out_dir, exist_ok=True)
    seeds = list(dict.fromkeys(args.seeds))
    scenarios = list(dict.fromkeys(args.scenarios))
    policies = list(dict.fromkeys(args.policies))
    unknown = [p for p in policies if p not in KNOWN_POLICIES]
    if unknown:
        fail(f"unknown policies {unknown}; known: {sorted(KNOWN_POLICIES)}")
    keep = parse_keep(args.keep_decisions, scenarios)
    run_policies = list(dict.fromkeys(policies + CHECK_POLICIES))
    check_only = [p for p in run_policies if p not in policies]

    gw, gt = load_generators(args.gen_dir)
    for sc in scenarios:
        for mod in (gw, gt):
            table = getattr(mod, "SCENARIOS", None)
            if table is not None and sc not in table:
                fail(f"scenario {sc!r} not in {mod.__file__} SCENARIOS ({sorted(table)})")
    cfg_paths = {}
    for sc in scenarios:
        p = os.path.join(os.path.abspath(args.scenarios_dir), f"{sc}.cfg")
        if not os.path.isfile(p):
            fail(f"{p}: scenario config missing")
        cfg_paths[sc] = p

    checks = {
        "label": "HOST SIMULATION",
        "ledger_fixtures": [],
        "ablation_defer0_bytes": [],
        "edf_rr_vs_fresh_nodefer_event_slots": [],
        "ablation_defer0_summary": [],
        "interval_violations_zero": [],
        "accounting_identity": [],
    }
    checks_path = os.path.join(out_dir, summarize.CHECKS)

    # 0. preflight ledger fixtures
    print("run_matrix: ledger regression fixtures (preflight) ...")
    fixtures = ledger_preflight(flsim, os.path.join(out_dir, "checks", "ledger_fixtures"), policies)
    checks["ledger_fixtures"] = fixtures
    failed = [f for f in fixtures if not f["passed"]]
    if failed:
        checks["all_passed"] = False
        summarize.write_json(checks_path, checks)
        for f in failed:
            print(f"  FAIL {f['fixture']} [{f['policy']}]: {f['mismatches']}", file=sys.stderr)
        fail(f"{len(failed)} ledger regression fixture(s) failed; matrix results are not accepted (see {checks_path})")
    print(f"  ok: {len(fixtures)} fixture checks passed")

    # 1-3. runs and checks
    n_runs = 0
    for sc in scenarios:
        for seed in seeds:
            in_dir = os.path.join(out_dir, "inputs", sc, summarize.seed_dir_name(seed))
            os.makedirs(in_dir, exist_ok=True)
            w_path = os.path.join(in_dir, "workload.csv")
            t_path = os.path.join(in_dir, "trace.csv")
            c_path = os.path.join(in_dir, f"{sc}.cfg")
            try:
                n_w = write_rows_csv(w_path, gw.generate_workload(sc, seed), WORKLOAD_HEADER)
                n_t = write_rows_csv(t_path, gt.generate_trace(sc, seed), TRACE_HEADER)
            except Exception as exc:  # generator contract violation: loud
                fail(f"generator failed for {sc} seed {seed}: {type(exc).__name__}: {exc}")
            shutil.copyfile(cfg_paths[sc], c_path)
            run_base = os.path.join(out_dir, "runs", sc, summarize.seed_dir_name(seed))
            for pol in run_policies:
                run_flsim(flsim, w_path, t_path, c_path, pol, os.path.join(run_base, pol))
                n_runs += 1
                got = read_summary(os.path.join(run_base, pol)).get("policy")
                if got != pol:
                    fail(f"{run_base}/{pol}/summary.csv: policy column is {got!r}, expected {pol!r}")
            defer0_dir = os.path.join(run_base, summarize.ABLATION_DIR)
            run_flsim(flsim, w_path, t_path, c_path, "fresh", defer0_dir, extra=("--defer", "0"))
            n_runs += 1

            nodefer_dir = os.path.join(run_base, "fresh_nodefer")
            edf_dir = os.path.join(run_base, "edf_rr")
            ok, detail = compare_bytes(os.path.join(defer0_dir, "decisions.csv"), os.path.join(nodefer_dir, "decisions.csv"))
            checks["ablation_defer0_bytes"].append({"scenario": sc, "seed": seed, "passed": ok, "detail": detail})
            res = compare_event_slots(os.path.join(edf_dir, "decisions.csv"), os.path.join(nodefer_dir, "decisions.csv"))
            res.update({"scenario": sc, "seed": seed})
            checks["edf_rr_vs_fresh_nodefer_event_slots"].append(res)
            ok, diffs = compare_summaries(defer0_dir, nodefer_dir)
            checks["ablation_defer0_summary"].append({"scenario": sc, "seed": seed, "passed": ok, "diffs": diffs})
            for pol in run_policies:
                s = read_summary(os.path.join(run_base, pol))
                iv = int(s["interval_violations"])
                checks["interval_violations_zero"].append(
                    {"scenario": sc, "seed": seed, "policy": pol, "passed": iv == 0, "interval_violations": iv}
                )
                ok, detail = accounting_identity(s)
                checks["accounting_identity"].append(
                    {"scenario": sc, "seed": seed, "policy": pol, "passed": ok, "detail": detail}
                )
            print(f"run_matrix: {sc} seed {seed}: {n_w} workload rows, {n_t} trace slots, {len(run_policies) + 1} runs")

    # 4. prune bulky per-slot logs
    removed = prune_run_dirs(os.path.join(out_dir, "runs"), keep)
    print(f"run_matrix: pruned {removed} decisions/rx files (kept for: {sorted(keep) or 'none'})")

    # 5. aggregate, plot, manifest
    manifest_extra = {"check_only_policies": check_only, "keep_decisions": sorted(keep), "n_runs": n_runs}
    all_ok = all(e["passed"] for name, lst in checks.items() if isinstance(lst, list) for e in lst)
    checks["all_passed"] = all_ok
    summarize.write_json(checks_path, checks)
    meta = {
        **summarize.git_state(REPO_DIR),
        "flsim_sha256": summarize.sha256_file(flsim),
        "seeds": seeds,
        "policies": policies,
        "scenarios": scenarios,
    }
    _, all_rows, _, agg_rows = summarize.aggregate_out_dir(
        out_dir, scenarios=scenarios, seeds=seeds, policies=policies, plot=True, meta=meta, checks=checks
    )
    manifest = summarize.build_manifest(
        out_dir, flsim=flsim, repo_dir=REPO_DIR, seeds=seeds, policies=policies, scenarios=scenarios, extra=manifest_extra
    )
    summarize.write_json(os.path.join(out_dir, summarize.MANIFEST), manifest)

    n_checks = sum(len(lst) for lst in checks.values() if isinstance(lst, list))
    n_failed = sum(1 for lst in checks.values() if isinstance(lst, list) for e in lst if not e["passed"])
    print(
        f"run_matrix: HOST SIMULATION matrix done: {n_runs} flsim runs, {len(all_rows)} aggregated rows, "
        f"{len(agg_rows)} scenario x policy rows, checks {n_checks - n_failed}/{n_checks} passed -> {out_dir}"
    )
    if not all_ok:
        for name, lst in checks.items():
            if isinstance(lst, list):
                for e in lst:
                    if not e["passed"]:
                        print(f"  FAIL {name}: {e}", file=sys.stderr)
        print("run_matrix: FAIL: one or more checks failed (see checks.json)", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
