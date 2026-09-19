# Pilot / development runs (HOST SIMULATION) — provenance record

These outputs are **development artefacts**, not evaluation results. They are
kept because a configuration decision was based on them (docs/DESIGN.md §10.2).

`alarm_outage_seed101_max_attempts8/`: deterministic re-run of the first
`alarm_outage` pilot (seed 101, the original common config with
`max_attempts=8`, `ack_timeout_ms=300`). Both alarm events end
`RETRY_EXHAUSTED` inside the 9–15 s outage (RAISE at 12 400 ms after 8
attempts, CLEAR at 14 400 ms), `rx_ev_delivered = 0`; the 17 s retention was
unreachable. This is the retry-budget limitation that led to
`max_attempts=40` for the two outage scenarios, applied identically to every
policy. Seeds 1–3 and 101–105 are development/pilot seeds (see §10.1).
