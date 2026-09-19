# Freshness Lab

Bounded-memory C11 telemetry core for **replaceable STATE snapshots** and
**non-replaceable EVENT occurrences**, with a deterministic **HOST SIMULATION**
harness for replaying matched fault traces and comparing scheduling policies.

> **Scope, stated plainly.** Everything measured here is a host simulation on
> a desktop CPU. No microcontroller has run this code. The scheduler
> comparison is an experiment with a possible negative result, not a claim of
> superiority over anything. This repository makes no world-first,
> university-affiliation, safety-certification, hardware-performance or
> universal-superiority claims. The code was written and tested with AI
> assistance (Claude Code) in this repository; commits carry that attribution.

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
| Counterexample reducer (ddmin over workload rows, trace and time axis fixed) | **Implemented** (see report for what it was run on) | `tools/reduce_counterexample.py` |
| Strict warnings (gcc + clang `-Weverything`), ASan/UBSan test run | **Implemented** | `Makefile` |

## What is planned (not implemented; do not cite as done)

* Port to a Cortex-M class board with a real UART/radio link; measure RAM,
  flash, cycles per step. **Milestone 2.**
* CRC trailer on frames; corruption (not only loss/delay) in the trace.
* Receiver-visible loss notification (so the receiver learns about
  `REJECTED_FULL` / expiry); currently accounting is local to each end.
* AoII-style metric; sender-side receiver-state estimator.
* Larger dedup windows or per-stream event ledgers; session negotiation.

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

Full specification: `docs/DESIGN.md`. Memory: `docs/MEMORY.md`. Prior art
and positioning: `docs/RELATED_WORK.md`. Results: `docs/REPORT.md`.

## Results

See `docs/REPORT.md` (generated from `results/matrix/`). Summary table and
plot: `results/matrix/summary.md`, `results/matrix/plot.svg`.

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
