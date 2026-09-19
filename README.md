# Freshness Lab

Bounded-memory C11 telemetry core for **replaceable STATE snapshots** and
**non-replaceable EVENT occurrences**, with a deterministic **HOST SIMULATION**
harness for replaying matched fault traces and comparing scheduling policies.

> **Scope, stated plainly.** This is a **software-only research and
> simulation project**: it runs entirely on a desktop, no board is required,
> and no hardware was used. Every number here is a host simulation on
> development seeds; the scheduler comparison is an experiment with a
> possible negative result, not a claim of superiority over anything. This
> repository makes no world-first, university-affiliation,
> safety-certification, hardware-performance or universal-superiority claims.
> The code was written and tested with AI assistance (Claude Code) in this
> repository; commits carry that attribution.

**Start here:** [`docs/RESULTS_SUMMARY.md`](docs/RESULTS_SUMMARY.md) (two-page
summary for a new reader) and [`docs/REPORT.md`](docs/REPORT.md) (full report
with walkthroughs, review reconciliation and the clean reproduction proof).

## The one-minute demo

An alarm raises at t = 10 s and clears at t = 12 s while the link is down from
9 s to 15 s (`alarm_outage` scenario).

* A **latest-state-only** receiver sees `alarm_active = 0` before the outage
  and `0` after it. The waiting snapshot with `1` was superseded by the `0`
  snapshot before the link came back. The occurrence is gone.
* The **event ledger** delivers `RAISE #1 (gen 10000)` and `CLEAR #2
  (gen 12000)` at about 15 s: late, flagged as deadline-missed, but recorded
  with their immutable IDs and generation times.

This is a statement about *data semantics*, not about which scheduler is
better. See `docs/REPORT.md` for the walkthrough with the actual trace.

## What is implemented (v0.1, this repository)

| Area | Status | Where |
|---|---|---|
| Fixed-memory sender core: per-stream latest/in-flight STATE, bounded EVENT slots, retention expiry, retry limit, ACK validation | **Implemented, tested** | `core/fl_sender.[ch]` |
| Fixed-memory receiver core: never-regress STATE, 64-ID dedup window with explicit out-of-window rejection, bounded reverse ACK queue | **Implemented, tested** | `core/fl_receiver.[ch]` |
| Exact-length little-endian wire codec, length checked before any read | **Implemented, tested** | `core/fl_frame.[ch]` |
| Policies: `edf_rr` (event EDF + state round robin), `fresh_nodefer` (ablation baseline), `fresh` (candidate, slack-guarded deferral), `fresh_so` (starvation-override ablation), `fifo`; `late_demote` option for every family | **Implemented, tested** | `core/fl_policy.[ch]` |
| Structural separation: policy sees only a read-only view built from the sender's own ledger | **Implemented** | `core/fl_policy.h` |
| Host harness: 1 ms loop, slot-indexed matched channel trace (loss + delay per direction), full-duplex one-frame-per-slot service, per-event ledger, decision log, receiver log | **Implemented, tested** | `host/sim.[ch]`, `host/csvio.c` |
| Continuous-time AoI accumulator (exact integer arithmetic), unknown time, peak, over-threshold time | **Implemented, tested against a hand fixture** | `host/aoi.[ch]` |
| Accounting identity checked on every run; ledger reconciliation; receiver-seq interval check | **Implemented, tested** | `host/sim.c`, `tests/test_sim.c` |
| Scenario generators (8 scenarios, seeded), matrix runner, aggregation, SVG plot, manifest with hashes | **Implemented** | `tools/` |
| Counterexample reducer (ddmin over workload rows, trace and time axis fixed) | **Implemented, run on the overload loss** (`results/reduce/`) | `tools/reduce_counterexample.py` |
| Strict warnings (gcc + clang `-Weverything`), ASan/UBSan test run | **Implemented** | `Makefile` |

## What is not done (do not cite as done)

Hardware is unavailable and outside the scope of this project: MCU timing,
power and flash have **not** been measured, and no board has run this code.
Software-only future work, none of it implemented:

* Unseen-seed evaluation after freezing the policies and configurations
  (the current matrix uses development seeds).
* Workload and budget sensitivity: event rate, deadline, retry budget,
  buffer capacity.
* Stronger adversarial fault combinations (overlapping outages, ACK-only
  loss during overload, reordering under overload).
* CRC trailer and corruption in the channel model; receiver-visible loss
  notification; an AoII-style metric; session negotiation.

## Build, test, reproduce

```
make                # build/flsim (gcc, -std=c11, strict warnings, -Werror)
make test           # all C test binaries
make sanitize       # same tests under ASan + UBSan (build-san/)
make clang-check    # clang -Weverything compile of core and host
make tools-test     # Python unittest for the tools (stdlib only)
make matrix         # HOST SIMULATION matrix -> results/matrix/
```

Single run:

```
python3 tools/gen_workload.py --scenario alarm_outage --seed 101 --out w.csv
python3 tools/gen_trace.py    --scenario alarm_outage --seed 101 --out t.csv
build/flsim --workload w.csv --trace t.csv --config scenarios/alarm_outage.cfg --policy edf_rr --out out/edf_rr
```

Outputs per run: `summary.csv`, `events.csv` (per-event ledger, both ends),
`decisions.csv` (per-slot decision and reason), `state.csv` (per-stream AoI),
`rx.csv`, `config.txt`, `runtime.txt` (host runtime only).

## Semantics in one screen

* **Time** is `uint32_t` ms supplied by the host; horizon 2^31 − 1 ms; no
  regress; relative durations ≤ 2^30 so `gen + rel` cannot wrap.
* **STATE**: newest waiting snapshot replaces older waiting ones; one
  in-flight frame per stream, never mutated; receiver applies only newer seq;
  ACK reports the receiver's applied seq at ACK time.
* **EVENT**: immutable ID assigned to every generated occurrence (rejected
  ones burn their ID so `generated = admitted + rejected`); deadline
  (quality target, receiver-side on-time check, inclusive) is distinct from
  retention (how long the sender keeps trying). Terminal outcomes:
  `REJECTED_FULL`, `ACKED`, `RETRY_EXHAUSTED`, `RETENTION_EXPIRED`, or
  `PENDING` (censored at run end). Late delivery is never counted as on time.
* **ACK** proves past receipt/application under these rules, not the
  receiver's current state. Receiver-delivered and sender-acknowledged
  ledgers are independent; `delivered_but_unacked` is reported.
* **Dedup scope**: 64 IDs behind the highest delivered; older IDs are
  rejected and counted, never guessed.
* **Budget**: every transmitted frame, lost or not, data or ACK, is charged
  its bytes. Bytes are not energy.

Full specification: [`docs/DESIGN.md`](docs/DESIGN.md). Memory:
[`docs/MEMORY.md`](docs/MEMORY.md). Prior art and positioning:
[`docs/RELATED_WORK.md`](docs/RELATED_WORK.md). Results:
[`docs/RESULTS_SUMMARY.md`](docs/RESULTS_SUMMARY.md) and
[`docs/REPORT.md`](docs/REPORT.md).

## Results (HOST SIMULATION, exploratory/pilot matrix)

Summary: [`docs/RESULTS_SUMMARY.md`](docs/RESULTS_SUMMARY.md). Full report:
[`docs/REPORT.md`](docs/REPORT.md). Tables:
[`results/matrix/summary.md`](results/matrix/summary.md); plot:
[`results/matrix/plot.svg`](results/matrix/plot.svg); provenance:
[`results/matrix/manifest.json`](results/matrix/manifest.json) and
[`results/reproduction_log.txt`](results/reproduction_log.txt)
(clean-worktree rebuild, tests, sanitizers, matrix regeneration with
identical hashes).

* 280 policy runs (7 policies × 8 scenarios × 5 pilot seeds) + 40 `fresh --defer 0`
  ablation runs = 320 runs; 697/697 consistency checks passed
  (accounting identity, ablation byte-equality, interval check on every run).
* **Semantics demo (`alarm_outage`):** the receiver never sees
  `alarm_active = 1` on the state stream; the event ledger delivers both
  occurrences late (0/2 on time, 2/2 recall) and in reversed arrival order.
  The ledger preserves history; it does not by itself establish correct
  reordered alarm-state application.
* **Explicit rejection (`overflow`):** 28/40 events delivered eventually,
  20/40 on time, 12 rejected at admission with burned IDs; identical for
  every policy.
* **Candidate `fresh` vs baselines:** near parity in these pilot runs on
  healthy, bursty, ACK-loss or reordering links; under persistent overload it
  **loses** about 1.2 percentage points of event recall (0.770 vs 0.782) while
  cutting mean AoI about five-fold (1210 vs 6232 ms); the scenario built to
  make it lose (`tight_deadline`) did not, and `fifo` had the highest on-time
  rate there with overlapping seed ranges. No superiority claim.
* **Negative finding kept:** with 8 attempts × 300 ms ACK timeout, events
  exhausted retries inside a 6 s outage and the declared 17 s retention was
  unreachable (`results/pilot/`); the outage scenarios now use 40 attempts
  for every policy.
* **Reduced counterexample:** `results/reduce/overload_seed101_recall/`
  shrinks the overload loss to 13 workload rows (1-minimal) with a
  side-by-side decision walkthrough.
* Seeds 101..105 are development seeds; an unseen-seed evaluation is
  software-only future work.

## Layout

```
core/       embedded core (no heap, no I/O)        host/      HOST SIMULATION harness
tests/      C tests + Python tool tests            tools/     generators, matrix, plot, reducer
scenarios/  per-scenario configs                   results/   retained raw outputs
docs/       DESIGN, REPORT, MEMORY, RELATED_WORK
```

## Attribution

Written with AI assistance (Claude Code) in this repository. Commits are
authored as `Claude <noreply@anthropic.com>` with a `Co-Authored-By` trailer
and session link; the repository owner has not claimed independent authorship
of this code. Sources credited in `docs/RELATED_WORK.md` were used as
references only; no code was copied from them.
