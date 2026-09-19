# Memory accounting (fixed capacities)

All core state lives in two objects the host allocates once (statically on a
target). Nothing in `core/` calls `malloc`, uses VLAs, recurses, or does I/O.

Measured with `gcc 13.3 -std=c11` on **x86-64 Linux** (natural alignment;
`uint64_t` counters force 8-byte alignment). Sizes on a 32-bit ARM EABI target
have not been measured (no cross compiler in this session); field layouts use
only 1/2/4/8-byte scalars so the totals are expected to be close, but that is
an expectation, not a measurement.

Default capacities: `FL_MAX_STREAMS=8`, `FL_EVENT_CAPACITY=8`,
`FL_RX_DEDUP_WINDOW=64`, `FL_RX_ACK_QUEUE=4`, payloads 8 bytes.

| Object | Bytes | Composition |
|---|---:|---|
| `fl_sender_t` | **888** | config 32 + 8 × stream 60 = 480 + 8 × event slot 36 = 288 + stats 80 + cursor/time 8 |
| `fl_receiver_t` | **296** | 8 × stream 20 = 160 + dedup (`highest_id` 4 + bitmap 8) + ACK ring 4 × 8 = 32 + per-stream ACK flags 8 + stats 64 + session/time 8 |
| stack, `fl_sender_step` | 192 + 260 + 28 | `fl_step_result_t` (caller-provided) 192, `fl_sched_view_t` 260, `fl_frame_t` 28 |
| stack, `fl_receiver_ingest` | 44 + 28 | `fl_rx_result_t` (caller-provided) 44, `fl_frame_t` 28 |
| code (`-Os`, x86-64 `.text`) | ≈ 8.2 KB | frame 930 + names 1003 + policy 1831 + receiver 1526 + sender 2943 (names table is optional on a target) |

How the numbers scale (per unit of capacity):
* one more state stream: +60 B sender, +21 B receiver (20 + ACK flag);
* one more event slot: +36 B sender, +≈1 B of step-result note capacity;
* dedup window is fixed at ≤ 64 IDs (one `uint64_t`);
* one more reverse ACK ring entry: +8 B receiver.

What is **not** in the core and lives on the host only (`host/`): the
per-event ledger (`ev_record_t` per generated event), decision log (one
record per slot), receiver log, AoI accumulators, in-transit frame queues,
workload and trace arrays. These are `malloc`ed by the HOST SIMULATION and
their size depends on the run length, not on the core.

Reproduce: `make` then run any scenario; `config.txt` in every output
directory prints `sizeof_sender` / `sizeof_receiver` for that build, and the
summary CSV carries the same two columns.
