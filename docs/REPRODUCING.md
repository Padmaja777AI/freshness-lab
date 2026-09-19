# Reproducing Freshness Lab (HOST SIMULATION)

Everything runs on an ordinary desktop. No board, radio or special hardware
is required or was used. This page gives the exact commands as supported by
the current `Makefile` and tools, the outputs to expect, and how to compare
a fresh run against the retained evidence.

## Prerequisites (verified)

| Need | Verified with | Notes |
|---|---|---|
| C11 compiler | gcc 13.3.0 (Ubuntu 24.04) | `make` uses `cc`; strict warnings and `-Werror` |
| GNU make | 4.3 | |
| Python 3 | 3.11.15 | standard library only; no packages to install |
| clang (optional) | 18.1.3 | only for `make clang-check`; its ASan runtime is not needed |
| Sanitizers (optional) | gcc `-fsanitize=address,undefined` | used by `make sanitize` |

## Get the code

The default branch `main` holds only the initial README. The complete
project lives on the working branch, so clone that branch explicitly:

```sh
git clone -b claude/dazzling-galileo-0800h2 https://github.com/Padmaja777AI/freshness-lab.git
cd freshness-lab
```

## Build and test

```sh
make                # build/flsim (host simulation runner)
make test           # six C suites: 4027 checks, all must pass
make sanitize       # same suites under ASan + UBSan, built in build-san/
make clang-check    # clang -Weverything compile of core/ and host/ (no link)
make tools-test     # 15 Python unittest cases for the tools
make check          # = test + tools-test
```

Expected tail of `make test`:

```
test_events: 247 checks, 0 failures
test_fixtures: 151 checks, 0 failures
test_frame_time: 163 checks, 0 failures
test_policy: 1849 checks, 0 failures
test_sim: 344 checks, 0 failures
test_state_rx: 1273 checks, 0 failures
ALL C TESTS PASSED
```

## A small demo: the alarm inside an outage

All demo output goes to `out/`, which is git-ignored, so nothing under
`results/` is touched.

```sh
mkdir -p out/demo
python3 tools/gen_workload.py --scenario alarm_outage --seed 101 --out out/demo/workload.csv
python3 tools/gen_trace.py    --scenario alarm_outage --seed 101 --out out/demo/trace.csv
for p in edf_rr fresh fifo; do
  build/flsim --workload out/demo/workload.csv --trace out/demo/trace.csv \
              --config scenarios/alarm_outage.cfg --policy $p --out out/demo/$p
done
```

Each run prints one summary line, for `edf_rr`:

```
HOST SIMULATION edf_rr: gen=2 adm=2 rej=0 acked=2 exh=0 exp=0 pend=0 | rx del=2 ontime=0 late=2 dup=0 oow=0 | aoi_mean_s0=1222.5 | bytes=4311 | interval_viol=0 core_err=0
```

and writes into `out/demo/<policy>/`:

| File | Content |
|---|---|
| `summary.csv` | one row of every metric (header + row) |
| `events.csv` | per-event ledger merged from both ends: outcome, attempts, first transmission, first receipt, on-time flag |
| `decisions.csv` | one row per 10 ms slot: what was sent and the policy's reason |
| `state.csv` | per-stream AoI (mean, area, peak, final age, unknown time) |
| `rx.csv` | every frame arrival at either end |
| `config.txt` | the resolved configuration, capacities and struct sizes |
| `runtime.txt` | host wall time only; not deterministic |

What to look for: in `events.csv` both events end `ACKED` with
`rx_on_time = 0` (CLEAR #2 delivered at 15020, RAISE #1 at 15120); in
`rx.csv` the stream-0 `STATE_APPLIED` rows jump from generation time 8000 to
15000 with no snapshot carrying value 1 in between; in `state.csv` stream 0
has `aoi_peak_ms = 7030`. `out/demo/edf_rr/events.csv` must be byte-identical
to the retained
`results/matrix/runs/alarm_outage/seed101/edf_rr/events.csv`:

```sh
cmp out/demo/edf_rr/events.csv results/matrix/runs/alarm_outage/seed101/edf_rr/events.csv && echo identical
```

Other scenarios: `python3 tools/gen_workload.py --list`. Policies:
`edf_rr edf_rr_ld fresh_nodefer fresh_nodefer_ld fresh fresh_ld fresh_so fifo fifo_ld`.
`flsim` also accepts `--defer 0|1`, `--late-demote 0|1`,
`--starvation-override 0|1` to override a preset's flags, and `--quiet`.

## The full matrix (320 runs)

`make matrix` writes to `results/matrix/` and would overwrite the retained
evidence. To reproduce without touching it, run the runner directly into
`out/`:

```sh
python3 tools/run_matrix.py --flsim build/flsim --out out/matrix
```

On the reference host (4-CPU x86-64 desktop) this took about 3 s of wall
time; treat that as an observation, not an expectation. It generates inputs
for 8 scenarios × 5 seeds (101..105), runs 7 policies
plus the `fresh --defer 0` ablation on each (320 runs), performs 697
consistency checks, prunes large logs except for `alarm_outage` and
`tight_deadline`, and writes `summary_all.csv`, `summary_by_scenario.csv`,
`summary.md`, `plot.svg`, `checks.json` and `manifest.json`. Expected last
line:

```
run_matrix: HOST SIMULATION matrix done: 320 flsim runs, 280 aggregated rows, 56 scenario x policy rows, checks 697/697 passed -> .../out/matrix
```

Options: `--seeds`, `--scenarios`, `--policies`, `--keep-decisions all|none|<list>`
(see `python3 tools/run_matrix.py --help`). `tools/summarize.py --out DIR`
re-aggregates an existing directory; `tools/plot_svg.py --in DIR/summary_by_scenario.csv --out DIR/plot.svg`
redraws the chart.

### Compare with the retained evidence

```sh
sha256sum out/matrix/summary_all.csv results/matrix/summary_all.csv
python3 - <<'PY'
import json
a = json.load(open("out/matrix/manifest.json"))["files"]
b = json.load(open("results/matrix/manifest.json"))["files"]
diff = [k for k in b if a.get(k, {}).get("sha256") != b[k]["sha256"]]
print(len(b), "manifest files,", len(diff), "differ")
PY
```

Expected: identical `summary_all.csv` hashes and `1080 manifest files, 0 differ`.

## The counterexample reducer

Shrinks the `overload` workload on which the candidate `fresh` has lower
event recall than `edf_rr`, keeping the trace, config and time axis fixed:

```sh
M=out/matrix/inputs/overload/seed101     # or results/matrix/inputs/overload/seed101
python3 tools/reduce_counterexample.py --flsim build/flsim \
  --workload $M/workload.csv --trace $M/trace.csv --config scenarios/overload.cfg \
  --baseline edf_rr --candidate fresh --metric recall \
  --out out/reduce/overload_seed101_recall --max-runs 300
```

Expected last line:

```
reduce_counterexample: reduced 2980 -> 13 rows in 148 flsim runs (73 predicate tests, 32 cache hits); 1-minimal: yes; after: baseline 1, candidate 0.916667. Outputs in .../out/reduce/overload_seed101_recall
```

`reduced_workload.csv` must match the retained
`results/reduce/overload_seed101_recall/reduced_workload.csv`;
`walkthrough.md` lists both policies' decisions side by side. Exit codes:
0 reduction written, 2 predicate does not hold on the full workload (also
used by argparse for usage errors), 1 input or `flsim` failure.

## Provenance

* Retained results were generated from source commit **`f18d875`**
  (`core/`, `host/`, `tools/`, `scenarios/`, `Makefile`, `tests/`). Later
  commits on the branch change only documentation, `results/` and
  `.gitignore`; `results/matrix/manifest.json` records `git_head`,
  `git_dirty_tracked = false` and the `flsim` binary hash.
* `results/reproduction_log.txt` is the log of a fresh detached worktree of
  that commit building, passing every test and sanitizer run, and
  regenerating the matrix and the reduced counterexample with identical
  hashes.
* Seeds 101..105 are development seeds (docs/DESIGN.md §10.1); the matrix is
  an exploratory pilot, not a held-out evaluation.

## What is deterministic and what is not

The files whose sha256 the manifest lists are deterministic outputs: the
generated inputs (`workload.csv`, `trace.csv`, the copied `.cfg`) and every
run's `summary.csv`, `events.csv`, `state.csv` (plus `decisions.csv` and
`rx.csv` where kept). They depend only on the seed, the scenario definition
and the code, and the clean-worktree reproduction in
`results/reproduction_log.txt` regenerated all 1080 of them with identical
hashes **in the reference environment** (Ubuntu 24.04, gcc 13.3.0, x86-64).
Equality on another compiler, platform or host has not been verified; the
manifest comparison above is the way to check it. Expected to differ between
runs or machines regardless: `runtime.txt` (host timing), the
`host_wall_seconds` in the reproduction log, and the path, timestamp and
git-state fields inside `manifest.json`, `reduction.json` and `checks.json`.
`summary.md` and `plot.svg` embed the git head and are identical only for
the same commit.

## Troubleshooting

* **`make sanitize` fails to link with clang** (`libclang_rt.asan` missing):
  the sanitizer target is verified with gcc; run `make sanitize CC=gcc`.
* **`make matrix` replaced `results/matrix/`:** it regenerates the same
  deterministic files from the same commit; `git status` will show only
  `runtime.txt`-free changes if the code is unchanged. Use
  `git checkout -- results/matrix` to restore the retained copy, and prefer
  `--out out/matrix` as above.
* **A run exits with code 3:** `summary.csv` has `core_error != 0` or
  `ledger_mismatch = 1`; this is the harness refusing to report an
  inconsistent run. Exit code 1 means an input file failed to load (the
  message names the row or key).
* **`workload row ... is outside run_ms`:** every workload row must have
  `time_ms < run_ms`; the harness rejects the run instead of silently
  ignoring rows.
* **Python tests need a writable `/tmp`** for temporary files; the C
  `test_sim` suite also uses `mkstemp` in `/tmp`.
* **Different compiler or platform:** the core and the run ledgers use
  fixed-width integer arithmetic, but the host AoI accumulator reports
  floating-point means and areas in `summary.csv` and `state.csv`, so
  equality on another platform is something to verify with the manifest
  comparison above, not to assume. Windows has not been tried.
