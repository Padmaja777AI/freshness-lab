#!/usr/bin/env python3
"""
reduce_counterexample.py — delta-debugging (ddmin) reducer for Freshness Lab
policy counterexamples.  HOST SIMULATION tool; Python 3.11 stdlib only.

PURPOSE
  Given a workload W, a matched channel trace T and a config C on which a
  CANDIDATE policy underperforms a BASELINE policy for one summary metric, find
  a much shorter workload on which the same comparison still holds, and write a
  side-by-side decision walkthrough so a reader can see *why* the candidate
  lost.  The tool never claims anything about which policy is better; it only
  shrinks an observed counterexample.

WHAT IS AND IS NOT CHANGED (environmental time axis is fixed)
  * The trace file and the config file are NEVER modified or rewritten.  Every
    flsim run — full or reduced — uses the very same trace and config paths, so
    slot k of the trace means the same loss/delay for every run (§8.2 matched
    trace) and run_ms is unchanged (§8.1).
  * Workload rows are ONLY REMOVED.  A kept row is written back byte-for-byte
    (the original line text), in the original file order, so times, streams,
    values, deadlines and retentions are never edited or re-timed.  Removing
    rows keeps the file sorted by time (§8.4).
  * Consequence for event IDs: IDs are assigned in workload order (§4.1), so a
    reduced workload renumbers events.  The walkthrough uses the IDs of the
    reduced run (identical for both policies, which see the same workload).

PREDICATE (fixed)
  holds(W') :=  candidate.metric(W')  is strictly WORSE than  baseline.metric(W')
  where "worse" depends on the metric's direction:
    higher is better (recall, on_time_rate, rx_ev_delivered, rx_ev_on_time,
      ev_acked)                                       -> candidate < baseline
    lower is better (aoi_mean_ms, aoi_peak_ms, lat_mean_ms, lat_max_ms,
      conf_mean_ms, conf_max_ms, rx_ev_late, rx_ev_duplicate,
      rx_ev_out_of_window, ev_retention_expired, ev_retry_exhausted,
      ev_rejected_full, ev_pending_end, delivered_but_unacked, unknown_ms_sum,
      over_threshold_ms_sum, event_retries, bytes_data_tx, bytes_ack_tx,
      bytes_total_tx, data_frames, ack_frames)        -> candidate > baseline
  Any other summary column needs an explicit --direction higher|lower.
  Both policies are run on the same rows; a run that exits non-zero (load
  error, core_error or ledger_mismatch, exit 1/3) makes the predicate FALSE for
  that subset (a broken run is never accepted as a counterexample) and is
  counted in reduction.json "eval_errors".
  On the FULL workload the predicate must hold, otherwise the tool exits 2
  ("no counterexample") without writing a reduction.

ALGORITHM
  Classic ddmin (Zeller & Hildebrandt 2002): granularity n starts at 2; test
  each of the n contiguous subsets (reduce to subset, n := 2), then each
  complement (reduce to complement, n := max(n-1, 2)), else double n up to |c|;
  stop when n == |c| and nothing reduces (the result is then 1-minimal: no
  single remaining row can be removed while keeping the predicate).  Subset
  order is deterministic (file order), predicate results are cached by the
  tuple of kept row indices, and --max-runs bounds the flsim invocations spent
  on the search (the initial pair + 2 per uncached test).  When the budget runs
  out the current subset is written and "one_minimal" is false.
  --protect RAISE,CLEAR (any subset of STATE,RAISE,CLEAR) pins rows of those
  action types: they are always kept and never offered for removal, so the
  event under study survives; "one_minimal" is then relative to the removable
  rows.  ddmin assumes the empty removable set does not satisfy the predicate;
  with identical inputs both policies produce identical summaries, so a strict
  comparison is false there by construction.

OUTPUTS (DIR)
  reduced_workload.csv   kept rows, verbatim, original header
  baseline/  candidate/  flsim outputs of both policies on the reduced workload
  reduction.json         original/reduced row counts, kept indices, runs used,
                         metric values before/after, one_minimal flag, errors
  walkthrough.md         per remaining event: both policies' decisions from
                         gen_time to terminal/delivery time, side by side
  work/                  per-test scratch runs (deleted unless --keep-work)
  runtime.txt inside run directories is host runtime only and is never
  compared by this tool.

EXIT CODES
  0 reduction written; 2 predicate does not hold on the full workload (no
  counterexample); 1 usage, input or flsim failure on the full workload.

CLI
  python3 tools/reduce_counterexample.py --flsim build/flsim --workload W.csv
      --trace T.csv --config C.cfg --baseline edf_rr --candidate fresh
      --metric on_time_rate --out DIR [--max-runs 400] [--protect RAISE,CLEAR]
      [--direction higher|lower] [--keep-work] [--walkthrough-max-events N]

Importable:  ddmin(items, predicate, max_tests=None, stats=None) -> list
"""

import argparse
import csv
import hashlib
import json
import os
import shutil
import subprocess
import sys

HIGHER_IS_BETTER = frozenset(
    ["recall", "on_time_rate", "rx_ev_delivered", "rx_ev_on_time", "ev_acked"]
)
LOWER_IS_BETTER = frozenset(
    [
        "aoi_mean_ms", "aoi_peak_ms", "lat_mean_ms", "lat_max_ms", "conf_mean_ms", "conf_max_ms",
        "rx_ev_late", "rx_ev_duplicate", "rx_ev_out_of_window", "ev_retention_expired",
        "ev_retry_exhausted", "ev_rejected_full", "ev_pending_end", "delivered_but_unacked",
        "unknown_ms_sum", "over_threshold_ms_sum", "event_retries", "bytes_data_tx", "bytes_ack_tx",
        "bytes_total_tx", "data_frames", "ack_frames",
    ]
)
KNOWN_ACTIONS = ("STATE", "RAISE", "CLEAR")
POLICIES = (
    "edf_rr", "edf_rr_ld", "fresh_nodefer", "fresh_nodefer_ld", "fresh", "fresh_ld", "fresh_so", "fifo", "fifo_ld",
)


class BudgetExhausted(Exception):
    """Raised by a predicate (or by ddmin itself) when the test budget is spent."""


# --------------------------------------------------------------------------
# ddmin (importable, pure)
# --------------------------------------------------------------------------
def _chunks(positions, n):
    """Split a list into n contiguous, non-empty chunks (sizes differ by <= 1). Requires 1 <= n <= len."""
    total = len(positions)
    base, extra = divmod(total, n)
    out = []
    start = 0
    for i in range(n):
        size = base + (1 if i < extra else 0)
        out.append(positions[start:start + size])
        start += size
    return out


def ddmin(items, predicate, max_tests=None, stats=None):
    """Classic ddmin. `predicate(subset_list) -> bool` must hold on `items` (the
    caller establishes that); the empty list is assumed not to satisfy it.
    Returns the reduced list (a subsequence of `items`, original order).
    Deterministic: chunks are contiguous and tested in order; results are cached
    by the tuple of kept positions.  `max_tests` bounds uncached predicate
    calls; the predicate may also raise BudgetExhausted.  If `stats` is a dict
    it receives: tests, cache_hits, one_minimal, budget_exhausted, final_size."""
    items = list(items)
    cur = list(range(len(items)))
    cache = {}
    counters = {"tests": 0, "cache_hits": 0}

    def test(positions):
        key = tuple(positions)
        if key in cache:
            counters["cache_hits"] += 1
            return cache[key]
        if max_tests is not None and counters["tests"] >= max_tests:
            raise BudgetExhausted()
        counters["tests"] += 1
        result = bool(predicate([items[p] for p in positions]))
        cache[key] = result
        return result

    n = 2
    one_minimal = False
    exhausted = False
    try:
        while len(cur) >= 2:
            n = min(n, len(cur))
            subsets = _chunks(cur, n)
            reduced = False
            for sub in subsets:  # reduce to subset
                if test(sub):
                    cur = sub
                    n = 2
                    reduced = True
                    break
            if reduced:
                continue
            for sub in subsets:  # reduce to complement
                first = set(sub)
                comp = [p for p in cur if p not in first]
                if test(comp):
                    cur = comp
                    n = max(n - 1, 2)
                    reduced = True
                    break
            if reduced:
                continue
            if n < len(cur):
                n = min(2 * n, len(cur))
            else:
                one_minimal = True
                break
        else:
            one_minimal = True  # |cur| <= 1: nothing can be removed under the empty-set assumption
    except BudgetExhausted:
        exhausted = True
    if stats is not None:
        stats.update(
            tests=counters["tests"], cache_hits=counters["cache_hits"], one_minimal=one_minimal,
            budget_exhausted=exhausted, final_size=len(cur),
        )
    return [items[p] for p in cur]


# --------------------------------------------------------------------------
# helpers
# --------------------------------------------------------------------------
def fail(msg, code=1):
    sys.stderr.write(f"reduce_counterexample: {msg}\n")
    sys.exit(code)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fp:
        for block in iter(lambda: fp.read(1 << 16), b""):
            h.update(block)
    return h.hexdigest()


def read_csv_dicts(path):
    with open(path, newline="", encoding="utf-8") as fp:
        rows = list(csv.reader(fp))
    if not rows:
        raise ValueError(f"{path}: empty CSV")
    header = rows[0]
    out = []
    for lineno, r in enumerate(rows[1:], start=2):
        if not r:
            continue
        if len(r) != len(header):
            raise ValueError(f"{path}:{lineno}: expected {len(header)} fields, got {len(r)}")
        out.append(dict(zip(header, r)))
    return header, out


def load_workload_rows(path):
    """Return (header_line, rows). rows = list of dicts {raw, time_ms, action, index}.
    Mirrors host/csvio.c: blank lines, '#' comments and a 'time_ms' header are not rows."""
    header_line = "time_ms,action,a,b,c"
    rows = []
    with open(path, encoding="utf-8") as fp:
        for line in fp:
            text = line.rstrip("\r\n")
            stripped = text.strip()
            if stripped == "" or stripped.startswith("#"):
                continue
            if stripped.startswith("time_ms"):
                header_line = text
                continue
            fields = [f.strip() for f in stripped.split(",")]
            if len(fields) < 3:
                raise ValueError(f"{path}: bad workload row: {text!r}")
            try:
                t = int(fields[0])
            except ValueError as exc:
                raise ValueError(f"{path}: bad time in row {text!r}") from exc
            action = fields[1]
            if action not in KNOWN_ACTIONS:
                raise ValueError(f"{path}: unknown action {action!r} in row {text!r}")
            rows.append({"raw": text, "time_ms": t, "action": action, "index": len(rows)})
    return header_line, rows


def write_workload(path, header_line, rows):
    with open(path, "w", encoding="utf-8", newline="\n") as fp:
        fp.write(header_line + "\n")
        for r in rows:
            fp.write(r["raw"] + "\n")


def metric_direction(metric, override):
    if override == "higher":
        return "higher_is_better"
    if override == "lower":
        return "lower_is_better"
    if metric in HIGHER_IS_BETTER:
        return "higher_is_better"
    if metric in LOWER_IS_BETTER:
        return "lower_is_better"
    return None


def predicate_holds(direction, base_val, cand_val):
    """Strict comparison; an undefined (None / NA) value on either side never 'holds'."""
    if base_val is None or cand_val is None:
        return False
    if direction == "higher_is_better":
        return cand_val < base_val
    return cand_val > base_val


def to_int_or_none(s):
    return None if s in ("-", "", None) else int(s)


# --------------------------------------------------------------------------
# flsim runner with budget
# --------------------------------------------------------------------------
class FlsimRunner:
    def __init__(self, flsim, trace, config, metric, max_runs):
        self.flsim = flsim
        self.trace = trace
        self.config = config
        self.metric = metric
        self.max_runs = max_runs
        self.runs_used = 0
        self.log = []

    def run(self, workload, policy, out_dir, count=True):
        """Run one policy. Returns (returncode, summary_row_or_None, metric_value_or_None, stderr)."""
        if count:
            if self.runs_used + 1 > self.max_runs:
                raise BudgetExhausted()
            self.runs_used += 1
        os.makedirs(out_dir, exist_ok=True)
        cmd = [
            self.flsim, "--workload", workload, "--trace", self.trace, "--config", self.config,
            "--policy", policy, "--out", out_dir, "--quiet",
        ]
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
        except OSError as exc:
            fail(f"cannot execute {self.flsim}: {exc}")
        if proc.returncode != 0:
            return proc.returncode, None, None, proc.stderr.strip()
        try:
            _, rows = read_csv_dicts(os.path.join(out_dir, "summary.csv"))
        except (OSError, ValueError) as exc:
            return -1, None, None, f"summary.csv unreadable: {exc}"
        if len(rows) != 1:
            return -1, None, None, f"summary.csv: expected 1 row, got {len(rows)}"
        row = rows[0]
        if self.metric not in row:
            fail(f"metric {self.metric!r} is not a summary.csv column; columns: {', '.join(row.keys())}")
        raw = str(row[self.metric]).strip()
        if raw.upper() in ("NA", "N/A", "NAN", ""):
            # explicit undefined metric (e.g. aoi_mean_ms with a never-received stream):
            # not an error of the run, but no comparison is possible on it
            return 0, row, None, f"metric {self.metric} is undefined (NA) for this run"
        try:
            value = float(raw)
        except ValueError:
            return -1, row, None, f"metric {self.metric} is not numeric: {row[self.metric]!r}"
        return 0, row, value, proc.stderr.strip()

    def run_pair(self, workload, baseline, candidate, base_dir, cand_dir, count=True):
        """Run both policies; the budget check covers the pair up front so a pair is never split."""
        if count and self.runs_used + 2 > self.max_runs:
            raise BudgetExhausted()
        rc_b, row_b, val_b, err_b = self.run(workload, baseline, base_dir, count)
        rc_c, row_c, val_c, err_c = self.run(workload, candidate, cand_dir, count)
        return {
            "baseline": {"rc": rc_b, "row": row_b, "value": val_b, "stderr": err_b},
            "candidate": {"rc": rc_c, "row": row_c, "value": val_c, "stderr": err_c},
        }


# --------------------------------------------------------------------------
# walkthrough
# --------------------------------------------------------------------------
def _fmt_refs(prefix, refs):
    """Compact list of refs: 's0-s7' when consecutive ascending, else a short comma list."""
    if not refs:
        return ""
    uniq = []
    for r in refs:
        if r not in uniq:
            uniq.append(r)
    if len(uniq) == 1:
        return f"{prefix}{uniq[0]}"
    consecutive = all(uniq[i] + 1 == uniq[i + 1] for i in range(len(uniq) - 1))
    if consecutive:
        return f"{prefix}{uniq[0]}-{prefix}{uniq[-1]}"
    if len(uniq) > 6:
        return ",".join(f"{prefix}{r}" for r in uniq[:5]) + f",...({len(uniq)} distinct)"
    return ",".join(f"{prefix}{r}" for r in uniq)


def _cell_signature(d, event_id):
    kind = d["kind"]
    ref = int(d["ref"])
    lost = d["lost"] == "1"
    if kind == "NONE":
        return ("IDLE", d["reason"], False, None), None
    if kind == "EVENT":
        mine = ref == event_id
        return ("EVENT", d["reason"], lost, int(d["attempt"]) if mine else None), ref
    return ("STATE", d["reason"], lost, None), ref


def _render_cell(sig, refs, event_id):
    kind, reason, lost, attempt = sig
    if kind == "IDLE":
        return "IDLE" if reason == "IDLE" else f"IDLE ({reason})"
    if kind == "EVENT":
        text = f"EVENT {_fmt_refs('#', refs)} {reason}"
        if attempt is not None:
            text += f" attempt {attempt}"
        if lost:
            text += " LOST"
        if refs and len(set(refs)) == 1 and refs[0] == event_id:
            return f"**{text}**"
        return text
    text = f"STATE {_fmt_refs('s', refs)} {reason}"
    if lost:
        text += " LOST"
    return text


def _decisions_in_window(decisions, t0, t1):
    return [d for d in decisions if t0 <= int(d["t"]) <= t1]


def _side_by_side(dec_b, dec_c, event_id):
    """Fold consecutive slots whose (kind, reason, lost, own-attempt) signatures repeat in both columns."""
    by_slot_c = {d["slot"]: d for d in dec_c}
    rows = []
    for d_b in dec_b:
        d_c = by_slot_c.get(d_b["slot"])
        if d_c is None:
            continue
        sig_b, ref_b = _cell_signature(d_b, event_id)
        sig_c, ref_c = _cell_signature(d_c, event_id)
        slot = int(d_b["slot"])
        t = int(d_b["t"])
        if rows and rows[-1]["sig_b"] == sig_b and rows[-1]["sig_c"] == sig_c and rows[-1]["slot1"] + 1 == slot:
            last = rows[-1]
            last["slot1"] = slot
            last["t1"] = t
            if ref_b is not None:
                last["refs_b"].append(ref_b)
            if ref_c is not None:
                last["refs_c"].append(ref_c)
        else:
            rows.append(
                {
                    "slot0": slot, "slot1": slot, "t0": t, "t1": t, "sig_b": sig_b, "sig_c": sig_c,
                    "refs_b": [] if ref_b is None else [ref_b], "refs_c": [] if ref_c is None else [ref_c],
                }
            )
    lines = []
    for r in rows:
        slots = str(r["slot0"]) if r["slot0"] == r["slot1"] else f"{r['slot0']}-{r['slot1']}"
        ts = str(r["t0"]) if r["t0"] == r["t1"] else f"{r['t0']}-{r['t1']}"
        cb = _render_cell(r["sig_b"], r["refs_b"], event_id)
        cc = _render_cell(r["sig_c"], r["refs_c"], event_id)
        lines.append(f"| {slots} | {ts} | {cb} | {cc} |")
    return lines


def _event_end(ev, run_end):
    end = int(ev["gen_time"])
    for key in ("terminal_time", "rx_first_time"):
        v = to_int_or_none(ev[key])
        if v is not None and v > end:
            end = v
    if to_int_or_none(ev["terminal_time"]) is None:
        end = max(end, run_end)
    return end


def _reason_histogram(decisions, t0, t1):
    hist = {}
    for d in decisions:
        t = int(d["t"])
        if t0 <= t < t1:
            key = f"{d['kind']}/{d['reason']}" if d["kind"] != "NONE" else "IDLE"
            hist[key] = hist.get(key, 0) + 1
    return ", ".join(f"{k} x{v}" for k, v in sorted(hist.items(), key=lambda kv: (-kv[1], kv[0])))


def _rx_notes(rx_rows, event_id):
    notes = []
    for r in rx_rows:
        if r["dir"] == "DATA" and r["outcome"].startswith("EVENT_") and int(r["id"]) == event_id:
            extra = ""
            if r["outcome"] == "EVENT_DELIVERED":
                extra = " on time" if r["on_time"] == "1" else " LATE"
            notes.append(f"t={r['t']} receiver: {r['outcome']}{extra}")
        elif r["dir"] == "ACK" and r["outcome"] == "ACK_EVENT" and int(r["id"]) == event_id:
            err = "" if r["err"] in ("OK", "FL_OK") else f" ({r['err']})"
            notes.append(f"t={r['t']} sender: ACK_EVENT arrived{err}")
    return notes


def _event_summary_cell(ev):
    outcome = ev["outcome"]
    term = ev["terminal_time"]
    first_tx = ev["first_tx_time"]
    rx = ev["rx_first_time"]
    on_time = ev["rx_on_time"]
    rx_text = "never delivered" if rx == "-" else f"delivered t={rx} ({'on time' if on_time == '1' else 'LATE'})"
    tx_text = "never sent" if first_tx == "-" else f"first tx t={first_tx}"
    return f"{outcome}" + ("" if term == "-" else f" @t={term}") + f", attempts {ev['attempts']}, {tx_text}, {rx_text}"


def write_walkthrough(path, ctx):
    base_dir, cand_dir = ctx["base_dir"], ctx["cand_dir"]
    _, ev_b = read_csv_dicts(os.path.join(base_dir, "events.csv"))
    _, ev_c = read_csv_dicts(os.path.join(cand_dir, "events.csv"))
    _, dec_b = read_csv_dicts(os.path.join(base_dir, "decisions.csv"))
    _, dec_c = read_csv_dicts(os.path.join(cand_dir, "decisions.csv"))
    _, rx_b = read_csv_dicts(os.path.join(base_dir, "rx.csv"))
    _, rx_c = read_csv_dicts(os.path.join(cand_dir, "rx.csv"))
    run_ms = int(ctx["after"]["baseline_row"]["run_ms"])
    slot_ms = int(ctx["after"]["baseline_row"]["slot_ms"])
    run_end = run_ms - 1
    ev_c_by_id = {e["id"]: e for e in ev_c}
    max_events = ctx["walkthrough_max_events"]

    L = []
    L.append("# Counterexample walkthrough (HOST SIMULATION)")
    L.append("")
    L.append("Generated by `tools/reduce_counterexample.py`. This is a mechanical listing of the")
    L.append("harness's decision log for a reduced workload; it makes no claim about which policy")
    L.append("is better in general.")
    L.append("")
    L.append(f"* baseline: `{ctx['baseline']}`  candidate: `{ctx['candidate']}`  metric: `{ctx['metric']}` ({ctx['direction']})")
    L.append(f"* predicate: `{ctx['predicate_text']}`")
    L.append(f"* full workload: {ctx['original_rows']} rows -> baseline {ctx['before']['baseline']:g}, "
             f"candidate {ctx['before']['candidate']:g}")
    L.append(f"* reduced workload: {ctx['reduced_rows']} rows -> baseline {ctx['after']['baseline']:g}, "
             f"candidate {ctx['after']['candidate']:g}")
    L.append(f"* flsim runs used in the search: {ctx['runs_used']} of {ctx['max_runs']}; "
             f"1-minimal over removable rows: {'yes' if ctx['one_minimal'] else 'NO (budget exhausted)'}")
    L.append(f"* protected action types (never removed): {', '.join(ctx['protect']) if ctx['protect'] else 'none'}")
    L.append(f"* trace and config unchanged: `{os.path.basename(ctx['trace'])}` (sha256 {ctx['trace_sha256'][:12]}...), "
             f"`{os.path.basename(ctx['config'])}` (sha256 {ctx['config_sha256'][:12]}...), run_ms={run_ms}, slot_ms={slot_ms}")
    L.append("")
    L.append("## Remaining workload rows")
    L.append("")
    L.append("```")
    L.append(ctx["header_line"])
    for r in ctx["kept_rows"]:
        L.append(r["raw"])
    L.append("```")
    L.append("")
    L.append("## Events (reduced run IDs)")
    L.append("")
    L.append("Cells: `EVENT #id REASON attempt k` (bold = the event of this section), `STATE sN REASON`, `IDLE`;")
    L.append("`LOST` marks a slot whose DATA frame the trace loses. Consecutive slots with the same")
    L.append("kind/reason in both columns are folded into one row; the ref list shows the streams/events served.")
    L.append("")
    if not ev_b:
        L.append("No events remain in the reduced workload.")
    shown = 0
    for e_b in ev_b:
        e_c = ev_c_by_id.get(e_b["id"])
        if e_c is None:
            continue
        if shown >= max_events:
            L.append(f"... {len(ev_b) - shown} more events omitted (--walkthrough-max-events {max_events}).")
            break
        shown += 1
        eid = int(e_b["id"])
        gen = int(e_b["gen_time"])
        L.append(f"### Event #{eid}: {e_b['kind']} code {e_b['code']}, gen_time {gen}, deadline_abs {e_b['deadline_abs']}, "
                 f"retention_abs {e_b['retention_abs']}, admitted {e_b['admitted']}")
        L.append("")
        L.append(f"| | baseline `{ctx['baseline']}` | candidate `{ctx['candidate']}` |")
        L.append("|---|---|---|")
        L.append(f"| outcome | {_event_summary_cell(e_b)} | {_event_summary_cell(e_c)} |")
        fb = to_int_or_none(e_b["first_tx_time"])
        fc = to_int_or_none(e_c["first_tx_time"])
        hb = _reason_histogram(dec_b, gen, fb if fb is not None else run_end + 1) or "(none)"
        hc = _reason_histogram(dec_c, gen, fc if fc is not None else run_end + 1) or "(none)"
        L.append(f"| slots before first tx | {hb} | {hc} |")
        nb = _rx_notes(rx_b, eid)
        nc = _rx_notes(rx_c, eid)
        L.append(f"| receiver / ACK timeline | {'; '.join(nb) if nb else '(nothing received)'} | "
                 f"{'; '.join(nc) if nc else '(nothing received)'} |")
        L.append("")
        end = max(_event_end(e_b, run_end), _event_end(e_c, run_end))
        L.append(f"Decisions from t={gen} to t={end} (both policies, same trace slots):")
        L.append("")
        L.append(f"| slot | t | baseline `{ctx['baseline']}` | candidate `{ctx['candidate']}` |")
        L.append("|---|---|---|---|")
        L.extend(_side_by_side(_decisions_in_window(dec_b, gen, end), _decisions_in_window(dec_c, gen, end), eid))
        L.append("")
    with open(path, "w", encoding="utf-8", newline="\n") as fp:
        fp.write("\n".join(L) + "\n")


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------
def parse_args(argv):
    ap = argparse.ArgumentParser(
        description="ddmin reducer for a policy counterexample (HOST SIMULATION tool; stdlib only)."
    )
    ap.add_argument("--flsim", required=True, help="path to the flsim binary (build/flsim)")
    ap.add_argument("--workload", required=True)
    ap.add_argument("--trace", required=True, help="matched channel trace; never modified")
    ap.add_argument("--config", required=True, help="scenario config; never modified")
    ap.add_argument("--baseline", required=True, choices=POLICIES)
    ap.add_argument("--candidate", required=True, choices=POLICIES)
    ap.add_argument("--metric", required=True, help="summary.csv column, e.g. on_time_rate")
    ap.add_argument("--out", required=True, help="output directory")
    ap.add_argument("--max-runs", type=int, default=400, help="flsim invocation budget for the search (default 400)")
    ap.add_argument("--protect", default="", help="comma list of action types never removed, e.g. RAISE,CLEAR")
    ap.add_argument("--direction", choices=("higher", "lower"), default=None,
                    help="override the metric direction (higher/lower is better)")
    ap.add_argument("--keep-work", action="store_true", help="keep DIR/work with every evaluated run")
    ap.add_argument("--walkthrough-max-events", type=int, default=50)
    return ap.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    flsim = os.path.abspath(args.flsim)
    if not (os.path.isfile(flsim) and os.access(flsim, os.X_OK)):
        fail(f"{flsim}: flsim binary missing or not executable (build it with 'make BUILD=build-tools build-tools/flsim')")
    for p in (args.workload, args.trace, args.config):
        if not os.path.isfile(p):
            fail(f"{p}: no such file")
    if args.baseline == args.candidate:
        fail("baseline and candidate must differ")
    if args.max_runs < 2:
        fail("--max-runs must be at least 2 (the full-workload pair)")
    direction = metric_direction(args.metric, args.direction)
    if direction is None:
        fail(f"metric {args.metric!r} has no known direction; pass --direction higher|lower")
    protect = [p.strip() for p in args.protect.split(",") if p.strip()]
    for p in protect:
        if p not in KNOWN_ACTIONS:
            fail(f"--protect: unknown action type {p!r} (choose from {', '.join(KNOWN_ACTIONS)})")

    workload = os.path.abspath(args.workload)
    trace = os.path.abspath(args.trace)
    config = os.path.abspath(args.config)
    out_dir = os.path.abspath(args.out)
    work_dir = os.path.join(out_dir, "work")
    os.makedirs(out_dir, exist_ok=True)
    for sub in ("work", "baseline", "candidate"):
        d = os.path.join(out_dir, sub)
        if os.path.isdir(d):
            shutil.rmtree(d)
    os.makedirs(work_dir)

    try:
        header_line, rows = load_workload_rows(workload)
    except (OSError, ValueError) as exc:
        fail(str(exc))
    if not rows:
        fail(f"{workload}: no workload rows")

    runner = FlsimRunner(flsim, trace, config, args.metric, args.max_runs)
    cmp_symbol = "<" if direction == "higher_is_better" else ">"
    predicate_text = f"candidate.{args.metric} {cmp_symbol} baseline.{args.metric}"

    # 1. full workload
    full_dir = os.path.join(work_dir, "full")
    res = runner.run_pair(workload, args.baseline, args.candidate,
                          os.path.join(full_dir, "baseline"), os.path.join(full_dir, "candidate"))
    for side in ("baseline", "candidate"):
        if res[side]["rc"] != 0:
            fail(f"flsim failed on the full workload for the {side} ({res[side]['rc']}): {res[side]['stderr']}")
    before_b, before_c = res["baseline"]["value"], res["candidate"]["value"]
    if before_b is None or before_c is None:
        sys.stderr.write(
            f"reduce_counterexample: metric {args.metric} is undefined (NA) on the full workload for "
            f"{'baseline' if before_b is None else 'candidate'}; no comparison is possible, nothing reduced.\n"
        )
        if not args.keep_work:
            shutil.rmtree(work_dir, ignore_errors=True)
        return 2
    if not predicate_holds(direction, before_b, before_c):
        sys.stderr.write(
            f"reduce_counterexample: no counterexample: {predicate_text} does not hold on the full workload "
            f"(baseline {before_b:g}, candidate {before_c:g}).\n"
        )
        if not args.keep_work:
            shutil.rmtree(work_dir, ignore_errors=True)
        return 2
    sys.stderr.write(
        f"reduce_counterexample: full workload {len(rows)} rows: baseline {args.metric}={before_b:g}, "
        f"candidate {args.metric}={before_c:g}; {predicate_text} holds. Reducing (HOST SIMULATION).\n"
    )

    # 2. ddmin over removable rows
    protected = [r for r in rows if r["action"] in protect]
    removable = [r for r in rows if r["action"] not in protect]
    cache = {}
    eval_log = []
    eval_errors = []
    state = {"eval_no": 0}

    def predicate(subset):
        kept = sorted(protected + subset, key=lambda r: r["index"])
        key = tuple(r["index"] for r in kept)
        if key in cache:
            return cache[key]
        state["eval_no"] += 1
        edir = os.path.join(work_dir, f"eval_{state['eval_no']:04d}")
        os.makedirs(edir)
        wpath = os.path.join(edir, "workload.csv")
        write_workload(wpath, header_line, kept)
        r = runner.run_pair(wpath, args.baseline, args.candidate,
                            os.path.join(edir, "baseline"), os.path.join(edir, "candidate"))
        holds = False
        if r["baseline"]["rc"] == 0 and r["candidate"]["rc"] == 0:
            holds = predicate_holds(direction, r["baseline"]["value"], r["candidate"]["value"])
        else:
            eval_errors.append(
                {"eval": state["eval_no"], "kept_rows": len(kept),
                 "baseline_rc": r["baseline"]["rc"], "candidate_rc": r["candidate"]["rc"],
                 "stderr": (r["baseline"]["stderr"] + " | " + r["candidate"]["stderr"]).strip(" |")}
            )
        eval_log.append(
            {"eval": state["eval_no"], "kept_rows": len(kept), "holds": holds,
             "baseline": r["baseline"]["value"], "candidate": r["candidate"]["value"]}
        )
        cache[key] = holds
        if not args.keep_work:
            shutil.rmtree(edir, ignore_errors=True)
        return holds

    stats = {}
    kept_removable = ddmin(removable, predicate, stats=stats)
    kept_rows = sorted(protected + kept_removable, key=lambda r: r["index"])
    one_minimal = bool(stats["one_minimal"])

    # 3. outputs on the reduced workload (final pair, outside the search budget)
    reduced_path = os.path.join(out_dir, "reduced_workload.csv")
    write_workload(reduced_path, header_line, kept_rows)
    base_dir = os.path.join(out_dir, "baseline")
    cand_dir = os.path.join(out_dir, "candidate")
    final = runner.run_pair(reduced_path, args.baseline, args.candidate, base_dir, cand_dir, count=False)
    for side in ("baseline", "candidate"):
        if final[side]["rc"] != 0:
            fail(f"flsim failed on the reduced workload for the {side} ({final[side]['rc']}): {final[side]['stderr']}")
    after_b, after_c = final["baseline"]["value"], final["candidate"]["value"]
    after_holds = predicate_holds(direction, after_b, after_c)
    if not after_holds:
        sys.stderr.write("reduce_counterexample: WARNING: predicate does not hold on the final re-run "
                         "(non-deterministic harness?); outputs written anyway.\n")

    ctx = {
        "baseline": args.baseline, "candidate": args.candidate, "metric": args.metric, "direction": direction,
        "predicate_text": predicate_text, "protect": protect, "trace": trace, "config": config,
        "trace_sha256": sha256_file(trace), "config_sha256": sha256_file(config),
        "original_rows": len(rows), "reduced_rows": len(kept_rows), "header_line": header_line,
        "kept_rows": kept_rows, "runs_used": runner.runs_used, "max_runs": args.max_runs,
        "one_minimal": one_minimal, "base_dir": base_dir, "cand_dir": cand_dir,
        "before": {"baseline": before_b, "candidate": before_c},
        "after": {"baseline": after_b, "candidate": after_c, "baseline_row": final["baseline"]["row"]},
        "walkthrough_max_events": args.walkthrough_max_events,
    }
    write_walkthrough(os.path.join(out_dir, "walkthrough.md"), ctx)

    reduction = {
        "tool": "tools/reduce_counterexample.py",
        "label": "HOST SIMULATION",
        "flsim": flsim,
        "flsim_sha256": sha256_file(flsim),
        "inputs": {
            "workload": workload, "workload_sha256": sha256_file(workload),
            "trace": trace, "trace_sha256": ctx["trace_sha256"],
            "config": config, "config_sha256": ctx["config_sha256"],
        },
        "trace_modified": False,
        "config_modified": False,
        "run_ms": int(final["baseline"]["row"]["run_ms"]),
        "baseline": args.baseline,
        "candidate": args.candidate,
        "metric": args.metric,
        "direction": direction,
        "predicate": predicate_text,
        "protect": protect,
        "original_rows": len(rows),
        "protected_rows": len(protected),
        "removable_rows": len(removable),
        "reduced_rows": len(kept_rows),
        "removed_rows": len(rows) - len(kept_rows),
        "kept_row_indices": [r["index"] for r in kept_rows],
        "before": {"baseline": before_b, "candidate": before_c, "holds": True},
        "after": {"baseline": after_b, "candidate": after_c, "holds": after_holds},
        "one_minimal": one_minimal,
        "budget_exhausted": bool(stats["budget_exhausted"]),
        "max_runs": args.max_runs,
        "runs_used": runner.runs_used,
        "final_runs_not_counted": 2,
        "flsim_runs_total": runner.runs_used + 2,
        "predicate_tests": stats["tests"],
        "ddmin_cache_hits": stats["cache_hits"],
        "eval_errors": eval_errors,
        "eval_log": eval_log,
        "outputs": {
            "reduced_workload": reduced_path, "baseline_dir": base_dir, "candidate_dir": cand_dir,
            "walkthrough": os.path.join(out_dir, "walkthrough.md"),
        },
        "note": "runtime.txt in run directories is host runtime only and is never compared.",
    }
    with open(os.path.join(out_dir, "reduction.json"), "w", encoding="utf-8", newline="\n") as fp:
        json.dump(reduction, fp, indent=2, sort_keys=True)
        fp.write("\n")
    if not args.keep_work:
        shutil.rmtree(work_dir, ignore_errors=True)
    sys.stderr.write(
        f"reduce_counterexample: reduced {len(rows)} -> {len(kept_rows)} rows in {runner.runs_used} flsim runs "
        f"({stats['tests']} predicate tests, {stats['cache_hits']} cache hits); "
        f"1-minimal: {'yes' if one_minimal else 'no (budget exhausted)'}; "
        f"after: baseline {after_b:g}, candidate {after_c:g}. Outputs in {out_dir}\n"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
