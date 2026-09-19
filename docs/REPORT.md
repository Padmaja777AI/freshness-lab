# Freshness Lab — Experiment report (HOST SIMULATION)

**Everything in this report is a host simulation** (`build/flsim`, x86-64,
gcc 13.3). No board, no radio, no real clock. Host runtime is reported only as
host runtime. Bytes are bytes, not energy.

Sections marked `[[FILL]]` are completed from `results/matrix/` at the end of
the run; nothing in them is written before the numbers exist.

## 1. What was run

* Commit: `[[FILL commit]]`; branch `claude/dazzling-galileo-0800h2`.
* Scenarios: `healthy_light`, `alarm_outage`, `burst_loss`, `ack_loss`,
  `reorder`, `overflow`, `overload`, `tight_deadline` (definitions:
  `tools/gen_workload.py`, `tools/gen_trace.py`, configs `scenarios/*.cfg`).
* Policies: `edf_rr`, `edf_rr_ld`, `fresh_nodefer`, `fresh` (candidate),
  `fresh_ld`, `fresh_so`, `fifo` (docs/DESIGN.md §9.5).
* Seeds: evaluation `101..105`; tuning `1..3` (used only while choosing the
  candidate's four parameters; never reported).
* Matched channel: every policy in a (scenario, seed) cell reads the same
  slot-indexed loss/delay trace in both directions (§8.2).
* Reproduce: `make && make matrix` (writes `results/matrix/`), or the
  commands in the README for a single run.

## 2. Definitions used in the tables (from docs/DESIGN.md)

* `recall` = receiver-delivered unique events / **all generated** events
  (rejected and undelivered ones count against it).
* `on_time_rate` = receiver deliveries with `rx_time <= deadline_abs` / all
  generated. Late deliveries never count.
* `aoi_mean_ms` = per-stream continuous-time mean AoI (§8.6), averaged over
  streams; `unknown_ms_sum` is time before the first snapshot; `over_threshold_ms_sum`
  is time with AoI above the scenario threshold.
* Sender terminal outcomes are exclusive and sum to `ev_generated`.
* `delivered_but_unacked` = receiver delivered it, sender never got the ACK.
* `bytes_total_tx` charges every transmitted frame, lost or not, both
  directions.

## 3. The demonstration: alarm raised and cleared inside an outage

`[[FILL walkthrough from results/matrix/runs/alarm_outage/seed101/*]]`

## 4. Matrix results

`[[FILL table from results/matrix/summary.md]]`

Plot: `results/matrix/plot.svg`.

## 5. Findings (measured, with regressions)

`[[FILL]]`

## 6. Ablation and consistency checks

`[[FILL from results/matrix/checks.json]]`

## 7. Counterexample reduction

`[[FILL or state that it is deferred to milestone 2]]`

## 8. Limitations

* Host simulation only; no MCU timing, RAM or flash measurements.
* Channel model: per-slot independent or two-state loss and fixed per-slot
  delay; no corruption, no partial frames, no clock drift.
* Reverse ACK service is one frame per slot with a 4-entry event ring; a
  different reverse model would change ACK-loss results.
* Sender knowledge is last-ACKed metadata only; no estimator.
* Dedup window 64 IDs; out-of-window IDs are refused, which turns into
  `RETRY_EXHAUSTED`/`RETENTION_EXPIRED` at the sender rather than delivery.
* Retry limit × ACK timeout bounds the effective lifetime of an event
  regardless of retention (see findings).
* No receiver-visible loss notification; accounting is local to each end.

## 9. Next milestone (planned, not done)

Port `core/` to a Cortex-M board with a UART link and a second board or a
host as receiver; measure RAM/flash/cycles per `fl_sender_step`; replay the
same traces through a real link emulator; add CRC and corruption.
