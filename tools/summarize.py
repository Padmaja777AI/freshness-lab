#!/usr/bin/env python3
"""
Freshness Lab -- aggregate HOST SIMULATION matrix results (docs/DESIGN.md S8.5, S9.5).

Importable functions used by tools/run_matrix.py, plus a small CLI that
re-aggregates an existing matrix output directory:

    python3 tools/summarize.py --out results/matrix [--no-plot]
                               [--refresh-manifest --flsim build/flsim]

Layout it reads (written by run_matrix.py):
    OUT/runs/<scenario>/seed<seed>/<policy>/summary.csv   (one row each)
    OUT/manifest.json                                     (ordering, optional)
Layout it writes:
    OUT/summary_all.csv          every run row, prefixed with scenario,seed
    OUT/summary_by_scenario.csv  per scenario x policy mean/min/max over seeds
    OUT/summary.md               human-readable tables (HOST SIMULATION label)
    OUT/plot.svg                 via tools/plot_svg.py (unless --no-plot)

Everything here is HOST SIMULATION bookkeeping on a desktop CPU.  Nothing is a
performance or superiority claim, and bytes are not energy (S8.3).
Standard library only.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import re
import subprocess
import sys

# Metrics aggregated per scenario x policy (mean, min, max over seeds).
AGG_METRICS = [
    "aoi_mean_defined_ms",
    "aoi_defined_streams",
    "recall",
    "on_time_rate",
    "rx_ev_late",
    "ev_rejected_full",
    "ev_retention_expired",
    "ev_retry_exhausted",
    "ev_pending_end",
    "delivered_but_unacked",
    "lat_mean_ms",
    "aoi_mean_ms",
    "aoi_peak_ms",
    "over_threshold_ms_sum",
    "unknown_ms_sum",
    "bytes_total_tx",
    "event_retries",
]

# Integer-valued columns of summary.csv among AGG_METRICS (host/csvio.c prints
# them with %u / %llu); the rest are printed as decimals.
INT_METRICS = {
    "aoi_defined_streams",
    "rx_ev_late",
    "ev_rejected_full",
    "ev_retention_expired",
    "ev_retry_exhausted",
    "ev_pending_end",
    "delivered_but_unacked",
    "aoi_peak_ms",
    "over_threshold_ms_sum",
    "unknown_ms_sum",
    "bytes_total_tx",
    "event_retries",
}

# Canonical policy order (DESIGN.md S9.5 presets first, then the other CLI names).
POLICY_ORDER = [
    "edf_rr",
    "edf_rr_ld",
    "fresh_nodefer",
    "fresh_nodefer_ld",
    "fresh",
    "fresh_ld",
    "fresh_so",
    "fifo",
    "fifo_ld",
]

# Ablation run directory (fresh --defer 0); never aggregated as a policy.
ABLATION_DIR = "fresh_defer0"

SUMMARY_ALL = "summary_all.csv"
SUMMARY_BY_SCENARIO = "summary_by_scenario.csv"
SUMMARY_MD = "summary.md"
PLOT_SVG = "plot.svg"
MANIFEST = "manifest.json"
CHECKS = "checks.json"


# --------------------------------------------------------------------------
# CSV / JSON helpers
# --------------------------------------------------------------------------
def read_csv(path):
    """Read a CSV with a header row -> (header list, list of dict rows)."""
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


def write_csv(path, header, rows):
    """Write dict (or sequence) rows with LF line endings (deterministic bytes)."""
    with open(path, "w", newline="", encoding="utf-8") as fp:
        w = csv.writer(fp, lineterminator="\n")
        w.writerow(header)
        for r in rows:
            if isinstance(r, dict):
                w.writerow([r.get(h, "") for h in header])
            else:
                w.writerow(list(r))


def write_json(path, obj):
    with open(path, "w", encoding="utf-8") as fp:
        json.dump(obj, fp, indent=2, sort_keys=True)
        fp.write("\n")


def read_json(path):
    with open(path, encoding="utf-8") as fp:
        return json.load(fp)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fp:
        for chunk in iter(lambda: fp.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def file_entry(path):
    return {"sha256": sha256_file(path), "size": os.path.getsize(path)}


def _git(repo_dir, *args):
    try:
        proc = subprocess.run(["git", "-C", repo_dir, *args], capture_output=True, text=True, check=False)
    except OSError:
        return None
    if proc.returncode != 0:
        return None
    return proc.stdout


def git_head(repo_dir):
    """Read-only: the commit the results were produced from (None if unavailable)."""
    out = _git(repo_dir, "rev-parse", "HEAD")
    return (out or "").strip() or None


def git_state(repo_dir):
    """Read-only provenance: HEAD, whether tracked files were modified, and the untracked count.

    A result set produced with git_dirty_tracked == True was NOT generated from the
    committed sources alone and must be regenerated before it is cited.
    """
    tracked = _git(repo_dir, "status", "--porcelain", "--untracked-files=no")
    untracked = _git(repo_dir, "status", "--porcelain", "--untracked-files=all")
    return {
        "git_head": git_head(repo_dir),
        "git_dirty_tracked": None if tracked is None else bool(tracked.strip()),
        "git_untracked_files": None
        if untracked is None
        else sum(1 for line in untracked.splitlines() if line.startswith("??")),
    }


# --------------------------------------------------------------------------
# Run discovery
# --------------------------------------------------------------------------
def seed_dir_name(seed):
    return f"seed{seed}"


def parse_seed_dir(name):
    m = re.fullmatch(r"seed(\d+)", name)
    return int(m.group(1)) if m else None


def sort_policies(names):
    """Canonical order first (S9.5), unknown names alphabetically after."""
    rank = {p: i for i, p in enumerate(POLICY_ORDER)}
    return sorted(names, key=lambda n: (rank.get(n, len(POLICY_ORDER)), n))


def _subdirs(path):
    return sorted(d for d in os.listdir(path) if os.path.isdir(os.path.join(path, d)))


def discover_runs(out_dir, scenarios=None, seeds=None, policies=None):
    """List (scenario, seed, policy, summary.csv path) in a deterministic order.

    When scenarios/seeds/policies are None they are discovered from the
    directory tree (scenarios alphabetically, seeds numerically, policies in
    canonical order, the ablation directory excluded).
    """
    runs_dir = os.path.join(out_dir, "runs")
    if not os.path.isdir(runs_dir):
        raise FileNotFoundError(f"{runs_dir}: no runs directory")
    if scenarios is None:
        scenarios = _subdirs(runs_dir)
    result = []
    for sc in scenarios:
        sc_dir = os.path.join(runs_dir, sc)
        if seeds is None:
            sc_seeds = sorted(s for s in (parse_seed_dir(d) for d in _subdirs(sc_dir)) if s is not None)
        else:
            sc_seeds = seeds
        for seed in sc_seeds:
            seed_dir = os.path.join(sc_dir, seed_dir_name(seed))
            if policies is None:
                pols = sort_policies([d for d in _subdirs(seed_dir) if d != ABLATION_DIR])
            else:
                pols = policies
            for pol in pols:
                path = os.path.join(seed_dir, pol, "summary.csv")
                if not os.path.isfile(path):
                    raise FileNotFoundError(f"{path}: missing run summary")
                result.append((sc, seed, pol, path))
    return result


# --------------------------------------------------------------------------
# Aggregation
# --------------------------------------------------------------------------
def collect_summary_rows(run_list):
    """Read every run's single summary.csv row -> (header, rows).

    The header is ['scenario', 'seed'] + the summary.csv header; row values
    are the exact strings flsim wrote (no re-formatting).
    """
    header = None
    first_path = None
    rows = []
    for sc, seed, pol, path in run_list:
        h, rs = read_csv(path)
        if len(rs) != 1:
            raise ValueError(f"{path}: expected exactly one summary row, got {len(rs)}")
        if header is None:
            header, first_path = h, path
        elif h != header:
            raise ValueError(f"{path}: summary.csv header differs from {first_path}")
        row = {"scenario": sc, "seed": str(seed), "run_policy": pol}
        row.update(rs[0])
        rows.append(row)
    return ["scenario", "seed"] + (header or []), rows


NA = "NA"


def num_or_none(s):
    """Parse a summary cell: None for the explicit NA marker (or empty), else float.

    A legitimate numeric 0 stays 0.0 (never classified as missing by truthiness).
    """
    if s is None:
        return None
    t = str(s).strip()
    if t == "" or t.upper() in ("NA", "N/A", "NAN"):
        return None
    return float(t)


def aggregate(rows, metrics=AGG_METRICS):
    """Per scenario x policy: mean/min/max over seeds -> (header, rows).

    Groups keep first-seen order. Means are printed with 6 decimals; min and
    max are the original strings from summary.csv (exact).
    """
    groups = {}
    order = []
    for r in rows:
        key = (r["scenario"], r["policy"])
        if key not in groups:
            groups[key] = []
            order.append(key)
        groups[key].append(r)
    header = ["scenario", "policy", "n_seeds", "seeds"]
    for m in metrics:
        header += [f"{m}_mean", f"{m}_min", f"{m}_max", f"{m}_n"]
    out = []
    for key in order:
        g = groups[key]
        row = {
            "scenario": key[0],
            "policy": key[1],
            "n_seeds": str(len(g)),
            "seeds": ";".join(r["seed"] for r in g),
        }
        for m in metrics:
            parsed = [(num_or_none(r.get(m)), r.get(m)) for r in g]
            vals = [(v, raw) for v, raw in parsed if v is not None]
            row[f"{m}_n"] = str(len(vals))
            if len(vals) < len(g):
                # A group with ANY undefined seed is reported as unavailable: a partial
                # mean over the defined seeds would silently favour one side. The
                # per-seed values remain in summary_all.csv.
                row[f"{m}_mean"] = NA
                row[f"{m}_min"] = NA
                row[f"{m}_max"] = NA
                continue
            mean = sum(v for v, _ in vals) / len(vals)
            vmin = min(vals, key=lambda t: t[0])
            vmax = max(vals, key=lambda t: t[0])
            row[f"{m}_mean"] = f"{mean:.6f}"
            row[f"{m}_min"] = vmin[1]
            row[f"{m}_max"] = vmax[1]
        out.append(row)
    return header, out


# --------------------------------------------------------------------------
# summary.md
# --------------------------------------------------------------------------
MD_TABLES = [
    (
        "Event ledger",
        [
            "recall",
            "on_time_rate",
            "rx_ev_late",
            "ev_rejected_full",
            "ev_retention_expired",
            "ev_retry_exhausted",
            "ev_pending_end",
            "delivered_but_unacked",
            "lat_mean_ms",
            "event_retries",
        ],
    ),
    (
        "State freshness and link bytes",
        ["aoi_mean_ms", "aoi_mean_defined_ms", "aoi_defined_streams", "aoi_peak_ms", "over_threshold_ms_sum",
         "unknown_ms_sum", "bytes_total_tx"],
    ),
]

METRIC_GLOSSARY = [
    ("recall", "rx_ev_delivered / ev_generated (denominator: all generated events, S8.5)"),
    ("on_time_rate", "rx_ev_on_time / ev_generated; on time iff rx_time <= deadline_abs (S7.3)"),
    ("rx_ev_late", "events delivered after their deadline (still delivered, never counted as on time)"),
    ("ev_rejected_full", "events generated while all FL_EVENT_CAPACITY slots were busy (ID burned, S6.3)"),
    ("ev_retention_expired", "events whose retention_abs passed before an ACK (S6.5)"),
    ("ev_retry_exhausted", "events whose last permitted attempt timed out (S6.5)"),
    ("ev_pending_end", "events still pending at the end of the run (censored, reported, S8.7)"),
    ("delivered_but_unacked", "receiver-delivered IDs whose sender outcome is not ACKED (S7.4)"),
    ("lat_mean_ms", "mean first-delivery latency rx_time - gen_time over delivered events"),
    ("event_retries", "event transmissions beyond the first attempt (S8.5)"),
    ("aoi_mean_ms", "time-weighted mean AoI at the receiver averaged over ALL configured streams; NA whenever any stream never received a snapshot (S8.6). Conditional on the interval after each stream's first reception: always read next to unknown_ms_sum"),
    ("aoi_mean_defined_ms", "PARTIAL aggregate over the streams that did receive a snapshot (coverage = aoi_defined_streams); not comparable as full-stream AoI"),
    ("aoi_defined_streams", "number of configured streams with at least one applied snapshot in the run"),
    ("aoi_peak_ms", "largest AoI reached on any stream (S8.6)"),
    ("over_threshold_ms_sum", "continuous-time ms with AoI > aoi_threshold_ms, summed over streams"),
    ("unknown_ms_sum", "ms before the first applied snapshot, summed over streams (warm-up, reported)"),
    ("bytes_total_tx", "bytes handed to the transport in both directions; bytes are not energy (S8.3)"),
]


def _fmt_value(metric, s):
    v = num_or_none(s)
    if v is None:
        return NA
    if metric in INT_METRICS:
        return str(int(round(v)))
    return f"{v:.4f}"


def _fmt_cell(metric, row):
    lo = num_or_none(row.get(f"{metric}_min"))
    hi = num_or_none(row.get(f"{metric}_max"))
    mean = num_or_none(row.get(f"{metric}_mean"))
    if mean is None or lo is None or hi is None:
        n_def = row.get(f"{metric}_n", "?")
        return f"NA ({n_def}/{row.get('n_seeds', '?')} seeds defined; no comparison)"
    if lo == hi:
        return _fmt_value(metric, row[f"{metric}_min"])
    mean_s = f"{mean:.1f}" if metric in INT_METRICS else f"{mean:.4f}"
    return f"{mean_s} [{_fmt_value(metric, row[f'{metric}_min'])}, {_fmt_value(metric, row[f'{metric}_max'])}]"


def _md_escape(s):
    return str(s).replace("|", "\\|")


# The first matrix is exploratory/pilot on the development seeds (DESIGN.md S10.1).
PILOT_SEEDS = [101, 102, 103, 104, 105]


def seed_label(seeds):
    """'101..105' for a contiguous run of seeds, else a comma list; None -> the pilot seeds."""
    if not seeds:
        seeds = PILOT_SEEDS
    seeds = sorted({int(x) for x in seeds})
    if len(seeds) > 1 and seeds == list(range(seeds[0], seeds[-1] + 1)):
        return f"{seeds[0]}..{seeds[-1]}"
    return ", ".join(str(x) for x in seeds)


def pilot_title(seeds=None):
    """Required title label: HOST SIMULATION, exploratory/pilot, development seeds, not held out."""
    return (
        f"HOST SIMULATION \u2014 exploratory/pilot matrix (development seeds {seed_label(seeds)}; "
        "not a held-out evaluation)"
    )


def render_summary_md(agg_rows, meta=None, checks=None):
    """Markdown text for summary.md (title labelled HOST SIMULATION, exploratory/pilot)."""
    meta = meta or {}
    seeds_used = meta.get("seeds")
    lines = []
    lines.append("# " + pilot_title(seeds_used))
    lines.append("")
    lines.append(
        "All numbers on this page are **HOST SIMULATION** results from `build/flsim` on a "
        "desktop CPU (docs/DESIGN.md scope label). No microcontroller board has run this code."
    )
    lines.append("")
    lines.append(
        "* **Exploratory / pilot matrix, not a held-out evaluation.** The seeds used here "
        f"({seed_label(seeds_used)}) are *development* seeds: they were used while building the "
        "generators, verifying the tools and inspecting the `alarm_outage` / `overflow` demos, and "
        "the `max_attempts=40` change for those two scenarios was made after looking at seed-101 "
        "output (DESIGN.md S10.1, S10.2). No preregistration is claimed. A held-out study on seeds "
        "chosen after this milestone and audited for prior use is a later milestone (*PLANNED*)."
    )
    if seeds_used and sorted({int(x) for x in seeds_used}) != PILOT_SEEDS:
        lines.append(
            f"* Note: this aggregate covers seeds {seed_label(seeds_used)} only; the full pilot matrix "
            f"design is seeds {seed_label(PILOT_SEEDS)}."
        )
    lines.append(
        "* **Bytes are not energy.** `bytes_total_tx` counts the bytes handed to the transport in "
        "both directions, first attempts and retries, delivered or lost (S8.3). It is not an "
        "energy, power or radio-time measurement."
    )
    lines.append(
        "* No superiority claim is made. Results are reported whether or not the candidate "
        "`fresh` wins (S9.5); the `alarm_outage` scenario is a statement about data semantics, "
        "not scheduler quality (S1)."
    )
    lines.append(
        "* Each cell is the mean over the pilot seeds with `[min, max]` across seeds in brackets "
        "(a single value when all seeds agree); `n` is the number of seeds. `recall` and "
        "`on_time_rate` use **all generated events** as denominator, including rejected and "
        "undelivered ones (S8.5)."
    )
    lines.append(
        "* All policies in one scenario x seed share the same workload, configuration and matched "
        "channel trace (S8.2); only the forward-frame choice differs. The candidate's four parameters "
        "were fixed from the link parameters before any comparison run and no tuning sweep was "
        "performed on any seed (S10.1)."
    )
    lines.append("")
    if meta:
        lines.append("## Provenance")
        lines.append("")
        for key in ("git_head", "git_dirty_tracked", "git_untracked_files", "flsim_sha256", "seeds", "policies", "scenarios"):
            if key in meta and meta[key] is not None:
                val = meta[key]
                if isinstance(val, (list, tuple)):
                    val = ", ".join(str(v) for v in val)
                lines.append(f"* `{key}`: {val}")
        lines.append("")
    if checks:
        lines.append("## Checks (from checks.json)")
        lines.append("")
        for name in sorted(checks):
            entries = checks[name]
            if not isinstance(entries, list):
                continue
            n_fail = sum(1 for e in entries if not e.get("passed"))
            status = "pass" if n_fail == 0 else f"FAIL ({n_fail} of {len(entries)})"
            lines.append(f"* `{name}`: {status} over {len(entries)} entries")
        lines.append("")
    scenarios = []
    for r in agg_rows:
        if r["scenario"] not in scenarios:
            scenarios.append(r["scenario"])
    if not scenarios:
        lines.append("_No runs found._")
        lines.append("")
    for sc in scenarios:
        rows = [r for r in agg_rows if r["scenario"] == sc]
        lines.append(f"## Scenario `{sc}`")
        lines.append("")
        for title, metrics in MD_TABLES:
            lines.append(f"### {title}")
            lines.append("")
            lines.append("| policy | n | " + " | ".join(metrics) + " |")
            lines.append("|---|---:|" + "|".join("---:" for _ in metrics) + "|")
            for r in rows:
                cells = [_fmt_cell(m, r) for m in metrics]
                lines.append(f"| `{_md_escape(r['policy'])}` | {r['n_seeds']} | " + " | ".join(cells) + " |")
            lines.append("")
    lines.append("## Metric glossary")
    lines.append("")
    for name, text in METRIC_GLOSSARY:
        lines.append(f"* `{name}`: {text}")
    lines.append("")
    lines.append(
        "_HOST SIMULATION only. Exploratory/pilot matrix on development seeds; not a held-out "
        "evaluation. Bytes are not energy. No performance or superiority claim._"
    )
    lines.append("")
    return "\n".join(lines)


def write_summary_md(agg_rows, path, meta=None, checks=None):
    text = render_summary_md(agg_rows, meta=meta, checks=checks)
    with open(path, "w", encoding="utf-8") as fp:
        fp.write(text)
    return text


# --------------------------------------------------------------------------
# Manifest
# --------------------------------------------------------------------------
RUN_MANIFEST_FILES = ("summary.csv", "events.csv", "state.csv")


def manifest_files(out_dir):
    """sha256/size of every input file and every summary/events/state.csv under runs/."""
    files = {}
    inputs_dir = os.path.join(out_dir, "inputs")
    if os.path.isdir(inputs_dir):
        for root, dirs, names in os.walk(inputs_dir):
            dirs.sort()
            for n in sorted(names):
                p = os.path.join(root, n)
                files[os.path.relpath(p, out_dir).replace(os.sep, "/")] = file_entry(p)
    runs_dir = os.path.join(out_dir, "runs")
    if os.path.isdir(runs_dir):
        for root, dirs, names in os.walk(runs_dir):
            dirs.sort()
            for n in sorted(names):
                if n in RUN_MANIFEST_FILES:
                    p = os.path.join(root, n)
                    files[os.path.relpath(p, out_dir).replace(os.sep, "/")] = file_entry(p)
    return files


def build_manifest(out_dir, flsim=None, repo_dir=None, seeds=None, policies=None, scenarios=None, extra=None):
    manifest = {
        "label": "HOST SIMULATION",
        "note": "runtime.txt is host runtime only (non-deterministic) and is excluded on purpose",
        "flsim": None,
        **(git_state(repo_dir) if repo_dir else {"git_head": None}),
        "seeds": list(seeds) if seeds is not None else None,
        "policies": list(policies) if policies is not None else None,
        "scenarios": list(scenarios) if scenarios is not None else None,
        "files": manifest_files(out_dir),
    }
    if flsim:
        manifest["flsim"] = {"path": os.path.abspath(flsim), **file_entry(flsim)}
    if extra:
        manifest.update(extra)
    return manifest


# --------------------------------------------------------------------------
# One-call aggregation of an OUT directory
# --------------------------------------------------------------------------
def aggregate_out_dir(out_dir, scenarios=None, seeds=None, policies=None, plot=True, meta=None, checks=None):
    """Write summary_all.csv, summary_by_scenario.csv, summary.md (and plot.svg).

    Returns (all_header, all_rows, agg_header, agg_rows).
    """
    run_list = discover_runs(out_dir, scenarios=scenarios, seeds=seeds, policies=policies)
    all_header, all_rows = collect_summary_rows(run_list)
    write_csv(os.path.join(out_dir, SUMMARY_ALL), all_header, all_rows)
    agg_header, agg_rows = aggregate(all_rows)
    write_csv(os.path.join(out_dir, SUMMARY_BY_SCENARIO), agg_header, agg_rows)
    write_summary_md(agg_rows, os.path.join(out_dir, SUMMARY_MD), meta=meta, checks=checks)
    if plot:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import plot_svg  # tools/plot_svg.py (stdlib only)

        plot_svg.write_plot(agg_rows, os.path.join(out_dir, PLOT_SVG))
    return all_header, all_rows, agg_header, agg_rows


def main(argv=None):
    ap = argparse.ArgumentParser(description="Re-aggregate a HOST SIMULATION matrix output directory.")
    ap.add_argument("--out", required=True, help="matrix output directory written by run_matrix.py")
    ap.add_argument("--no-plot", action="store_true", help="do not (re)write plot.svg")
    ap.add_argument("--refresh-manifest", action="store_true", help="rewrite manifest.json (needs --flsim)")
    ap.add_argument("--flsim", default=None, help="flsim binary (only for --refresh-manifest)")
    args = ap.parse_args(argv)

    out_dir = os.path.abspath(args.out)
    scenarios = seeds = policies = None
    meta = {}
    manifest_path = os.path.join(out_dir, MANIFEST)
    if os.path.isfile(manifest_path):
        m = read_json(manifest_path)
        scenarios, seeds, policies = m.get("scenarios"), m.get("seeds"), m.get("policies")
        meta = {
            "git_head": m.get("git_head"),
            "git_dirty_tracked": m.get("git_dirty_tracked"),
            "git_untracked_files": m.get("git_untracked_files"),
            "flsim_sha256": (m.get("flsim") or {}).get("sha256"),
            "seeds": seeds,
            "policies": policies,
            "scenarios": scenarios,
        }
    checks = None
    checks_path = os.path.join(out_dir, CHECKS)
    if os.path.isfile(checks_path):
        checks = read_json(checks_path)
    _, all_rows, _, agg_rows = aggregate_out_dir(
        out_dir, scenarios=scenarios, seeds=seeds, policies=policies, plot=not args.no_plot, meta=meta, checks=checks
    )
    if args.refresh_manifest:
        if not args.flsim:
            ap.error("--refresh-manifest needs --flsim")
        repo_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        write_json(
            manifest_path,
            build_manifest(out_dir, flsim=args.flsim, repo_dir=repo_dir, seeds=seeds, policies=policies, scenarios=scenarios),
        )
    print(f"HOST SIMULATION aggregate: {len(all_rows)} runs -> {len(agg_rows)} scenario x policy rows in {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
