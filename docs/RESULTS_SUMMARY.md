# Freshness Lab — results summary for a new reader

**What this is.** A software-only research and simulation project. A small
C11 telemetry core (fixed memory, no heap) sends two kinds of data over one
lossy link: **STATE** snapshots, where only the newest value matters, and
**EVENT** occurrences, where every occurrence matters. A deterministic host
harness replays scripted workloads against matched fault traces and compares
scheduling policies. Everything runs on a desktop; **no board is required
and no hardware was used**. All numbers below are HOST SIMULATION results
from five development seeds. They are a pilot, not a held-out proof.

## The question

When the link is bad, does a scheduler that may **defer an event with slack
to refresh stale state** (`fresh`, the candidate) change what the receiver
ends up knowing, compared with plain **event-first** scheduling
(`edf_rr`, the primary baseline: earliest-deadline events first, then
round-robin state)? And, independently of the scheduler: what does a
latest-state-only design lose that an event ledger keeps?

## What was tested

* 8 scripted scenarios (healthy light load, alarm raised and cleared inside an
  outage, burst loss, reverse ACK loss, reordering, event-buffer overflow,
  persistent overload, tight deadlines), each with its own configuration
  applied identically to every policy.
* 7 policies: `edf_rr` (primary baseline), `fresh_nodefer` (same state
  ranking as the candidate, never defers: the ablation baseline), `fresh`
  (candidate), `fresh_so` (candidate with the starvation guard allowed to
  outrank urgent events), `edf_rr_ld` and `fresh_ld` (late-event demotion),
  `fifo` (secondary).
* 320 runs: 280 policy runs (7 × 8 × 5 seeds) plus 40 `fresh --defer 0`
  ablation runs, which must reproduce `fresh_nodefer` byte for byte.

## Validation evidence

* 4027 C test checks in 6 suites, passing natively and under ASan/UBSan, plus
  15 Python tool tests. Hand-computed fixtures include the continuous-time
  AoI fixture (area 28, mean 2.8) and the lost-ACK ledger fixture
  (37 and 74 attempted bytes; 2 ms delivery vs 8 ms confirmation).
* 697 consistency assertions on the matrix, all passing: the accounting
  identity `generated = rejected + acked + exhausted + expired + pending` on
  every run, byte-identical ablation on all 40 cells, the receiver's applied
  sequence inside the sender's `[last_acked, max_sent]` interval at every
  millisecond, and per-event attempt reconciliation.
* Clean reproduction: a fresh checkout of the source commit `f18d875`
  rebuilt with `-Werror`, passed all tests and sanitizers, and regenerated
  the matrix with identical sha256 for all 1080 manifest files
  ([`results/reproduction_log.txt`](../results/reproduction_log.txt)).

## Headline numbers (means over 5 pilot seeds; denominator = all generated events)

"Eventual" counts an event delivered at any time; "on time" counts it only
if it arrived by its deadline. Mean AoI is the time-weighted age of the
receiver's state snapshot, conditional on the interval after each stream's
first reception and averaged over streams; it is always reported next to
the unknown time. Figures are rounded.

| Scenario | Policy | Eventual receipt | On-time receipt | Mean state age (AoI) | Note |
|---|---|---:|---:|---:|---|
| `healthy_light` | all 7 | 100 % | 100 % | ≈ 280 ms | no measurable difference; traffic within ≈ 1 % |
| `overload` | `edf_rr` | ≈ 78.2 % | ≈ 77.4 % | ≈ 6.2 s | state starves behind events |
| `overload` | `fresh` | ≈ 77.0 % | ≈ 76.2 % | ≈ 1.2 s (≈ 81 % lower) | about 1 pp fewer events, ≈ 5× fresher state |
| `overload` | `fifo` | ≈ 65.8 % | ≈ 65.1 % | ≈ 0.18 s | freshest state, most events lost |
| `alarm_outage` | all 7 | 2/2 | 0/2 | peak ≈ 7.0 s | both late; arrival order reversed |
| `overflow` | all 7 | 28/40 | 20/40 | peak ≈ 7.6 s | 12 rejected at admission, IDs burned |

Source cells: [`results/matrix/summary_by_scenario.csv`](../results/matrix/summary_by_scenario.csv)
(`recall_mean`, `on_time_rate_mean`, `aoi_mean_ms_mean`); full tables in
[`results/matrix/summary.md`](../results/matrix/summary.md); plot
[`results/matrix/plot.svg`](../results/matrix/plot.svg).

## Practical conclusions (what the data supports)

1. **Latest-state-only loses occurrences.** In the outage demo the receiver's
   state stream never shows `alarm_active = 1`; the event ledger delivers
   both occurrences late (0/2 on time) with correct IDs and generation times,
   but in reversed arrival order. The ledger preserves history; it does not by
   itself reconstruct the correct alarm state. This is a data-semantics
   result, independent of the scheduler.
2. **Capacity limits are explicit, not silent.** In overflow, 12 of 40 events
   are rejected at admission and counted; the 8 admitted during the outage
   arrive late but are recorded (28/40 eventual, 20/40 on time), identical
   for every policy.
3. **The candidate is a trade-off, not a win.** Under overload `fresh` gives
   up about one percentage point of event receipt (78.2 % → 77.0 % eventual,
   77.4 % → 76.2 % on time) for roughly 81 % lower mean state age. `fifo`
   pushes the same trade further (0.18 s age, 65.8 % receipt). On healthy,
   bursty, ACK-loss and reordering links no policy differs measurably. The
   scenario designed to make the candidate lose on deadlines did not; seed
   ranges overlap there and no ordering is claimed.
4. **The loss mechanism is auditable.** A deterministic reducer shrinks the
   overload loss to 13 workload rows: one first-ever state publish counts as
   "starved", the candidate spends one slot on it, and one later event finds
   the 8-slot buffer full
   ([`results/reduce/overload_seed101_recall/walkthrough.md`](../results/reduce/overload_seed101_recall/walkthrough.md)).
5. **Retry budget bounds event lifetime.** With 8 attempts × 300 ms, events
   exhausted retries inside a 6 s outage and the declared 17 s retention was
   unreachable ([`results/pilot/`](../results/pilot/README.md)); the outage
   scenarios use 40 attempts, for every policy.

The supported conclusion is an **auditable scheduling trade-off** between
event receipt and state freshness under overload, with bounded memory and
explicit failure accounting. It is not evidence of universal superiority
over any policy or protocol, and it is not a hardware measurement.

## Where the evidence lives

* Full report with walkthroughs and review reconciliation: [`REPORT.md`](REPORT.md)
* Normative design: [`DESIGN.md`](DESIGN.md); memory accounting: [`MEMORY.md`](MEMORY.md);
  prior art: [`RELATED_WORK.md`](RELATED_WORK.md)
* Retained per-run ledgers and inputs: [`results/matrix/`](../results/matrix/)
  (provenance in [`manifest.json`](../results/matrix/manifest.json), checks in
  [`checks.json`](../results/matrix/checks.json))
* Reproduction: `make && make test && make sanitize && make tools-test && make matrix`

## Future work (software-only; not implemented)

* Unseen-seed evaluation after freezing the policies and configurations.
* Workload and budget sensitivity (event rate, deadline, retry budget,
  buffer capacity).
* Stronger adversarial fault combinations (overlapping outages, ACK-only
  loss during overload, reordering under overload).
