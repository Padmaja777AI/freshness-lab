# Freshness Lab — Design Specification (vertical slice v0.1)

**Status:** implemented as described unless a section is marked *PLANNED*.
**Scope label:** everything measured in this repository is **HOST SIMULATION** on a
desktop CPU. No microcontroller board has run this code yet.

This document is the normative reference for the C11 core (`core/`), the host
harness (`host/`), the tests (`tests/`) and the experiment tooling (`tools/`).
Where code and this document disagree, that is a bug in one of them and a test
should catch it.

---

## 1. Problem statement

One **transmitter** owns several logical **STATE streams** and one **EVENT
ledger**. It talks to one **receiver** over a lossy, delaying, direction-
asymmetric link. The core must fit in fixed memory and never allocate on the
processing path.

Two kinds of telemetry have different semantics:

| Kind  | Semantics | Replaceable? | Loss consequence |
|-------|-----------|--------------|------------------|
| STATE | A snapshot of a value at a generation time. Only the newest matters. | Yes: a newer waiting snapshot supersedes an older waiting one. | The receiver's copy is older than it could be (Age of Information grows). |
| EVENT | An occurrence at a generation time with an immutable identity. Every occurrence matters. | No: raise and clear are separate occurrences and are never coalesced. | The occurrence is missing from the receiver's ledger. A late copy still has value. |

The compelling demonstration (`alarm_outage` scenario): an alarm raises and
clears while the link is down. A latest-state-only receiver sees `0` before
and `0` after the outage and has no record that the alarm ever fired. An event
ledger records both occurrences, late and flagged as deadline-missed.

**This demonstration is a statement about data semantics, not about scheduler
superiority.** The scheduler comparison in §9 is a separate, unproven question
and negative results are reported as such.

---

## 2. Non-goals and disclaimers

* Not a multi-node wireless MAC. One transmitter, one receiver, one logical link.
* Not a networking stack, not MQTT/DDS/CoAP compatible, not a reproduction of
  WiFresh or ACP+. See `docs/RELATED_WORK.md`.
* No real-time guarantee, no safety certification, no hardware performance
  claim, no universal-superiority claim.
* No persistence across reset: a reboot of either side is a **new session**
  (§4.2) and requires the host to re-initialise both ends.
* No Bayesian receiver-state estimator. Sender knowledge of the receiver is
  exactly the last ACKed metadata (§6.4).

---

## 3. Time, units, arithmetic

* `fl_time_t` is `uint32_t` **milliseconds**. Time origin `0` is the start of
  the session/run. The host supplies `now` explicitly to every core call.
* **Supported run horizon:** `0 <= now <= FL_TIME_HORIZON_MS = 2^31 - 1`
  (about 24.8 days). Any call with `now > FL_TIME_HORIZON_MS` returns
  `FL_ERR_TIME_HORIZON` and does nothing.
* **Monotonicity:** each core object records the last `now` it saw. A smaller
  `now` returns `FL_ERR_TIME_REGRESS` and does nothing. Equal `now` is allowed.
* Relative durations (`deadline_rel`, `retention_rel`, `ack_timeout_ms`, …)
  are bounded by `FL_MAX_REL_MS = 2^30`. Absolute times are computed as
  `gen_time + rel` and cannot overflow `uint32_t` because
  `(2^31 - 1) + 2^30 < 2^32`. This is asserted by boundary tests, not assumed.
* All "elapsed" computations are `now - earlier` on unsigned values where
  `now >= earlier` is guaranteed by monotonicity. No signed/unsigned mixing.
* Sentinel `FL_TIME_NONE = UINT32_MAX` means "no such time" (e.g. never sent,
  never ACKed). It is never a valid `now`.

---

## 4. Identity

### 4.1 Event occurrence ID
* `uint32_t`, session scoped, assigned by the sender core at *generation*
  (`fl_sender_post_event`). The first event of a session has ID `1`; `0` means
  "none".
* IDs are assigned to **every** generated event, including ones rejected
  because the buffer is full. Therefore `events_generated == highest_id` and
  accounting is exact: `generated = admitted + rejected_full`.
* An ID is never reused within a session and the record's identity fields
  (`id`, `kind`, `code`, `gen_time`, `deadline_abs`, `retention_abs`,
  `payload`) are never modified after generation.
* Exhaustion: when the next ID would be `UINT32_MAX`, `fl_sender_post_event`
  returns `FL_ERR_ID_EXHAUSTED` and generates nothing. (Reaching this needs
  more than 4×10^9 events in one session; it is a declared boundary, and
  tested by forcing the counter.)

### 4.2 Session
* `uint16_t session_id`, chosen by the host at init on both ends. Every frame
  carries it. The receiver rejects and counts frames whose session differs
  (`rx_session_mismatch`). There is no session negotiation and no persistence.

### 4.3 State sequence numbers
* Per stream `uint32_t seq`, session scoped, first snapshot has `seq = 1`.
* Exhaustion: when `seq == UINT32_MAX` the next publish returns
  `FL_ERR_SEQ_EXHAUSTED`. Within the horizon this needs >2^31 publishes on one
  stream, which is more than one per millisecond; declared and tested.

---

## 5. Wire format

All multi-byte integers are little-endian. Total lengths are exact per type;
the parser rejects any frame whose declared or actual length does not match.

```
Header (5 bytes): magic=0xF1 | type(1) | session(2) | len(1) = total bytes
type 1 STATE      : stream(1) seq(4) gen_time(4) payload(8)              -> 22 bytes
type 2 EVENT      : id(4) kind(1) code(1) gen_time(4) deadline_abs(4) payload(8) -> 27 bytes
type 3 ACK_STATE  : stream(1) applied_seq(4) applied_gen_time(4)        -> 14 bytes
type 4 ACK_EVENT  : id(4) flags(1)                                      -> 10 bytes
```

* `FL_MAX_FRAME_LEN = 32`. Any buffer shorter than the frame's declared length
  is a parse rejection (`FL_ERR_FRAME_LEN`), never a read past the buffer.
* Parser order: length >= 5 → magic → type known → `len` equals the type's
  exact size and equals the supplied byte count → session matches. Each
  rejection is counted separately in the receiver/sender stats.
* `ACK_EVENT.flags` bit0 = `FL_ACKF_DUPLICATE` (the receiver had already
  delivered this ID and suppressed the duplicate). Informational only.
* No CRC in v0.1 (*PLANNED*: CRC-8 trailer). The host simulation never
  corrupts bytes; it only loses or delays frames.
* **Retention is not on the wire.** The receiver does not need it.

Cost accounting: every frame handed to the transport is charged its full byte
length to the sending direction whether or not the channel later loses it
(§8.3).

---

## 6. Sender core

### 6.1 Capacities (compile-time, overridable with `-D`)

| Constant | Default | Meaning |
|---|---|---|
| `FL_MAX_STREAMS` | 8 | maximum logical state streams |
| `FL_EVENT_CAPACITY` | 8 | pending event slots (admitted, not yet terminal) |
| `FL_STATE_PAYLOAD_LEN` | 8 | bytes of state payload |
| `FL_EVENT_PAYLOAD_LEN` | 8 | bytes of event payload |
| `FL_RX_DEDUP_WINDOW` | 64 | receiver duplicate-detection window (IDs) |
| `FL_RX_ACK_QUEUE` | 4 | receiver ACK_EVENT ring capacity (reverse queue) |
| `FL_MAX_FRAME_LEN` | 32 | largest frame either side will encode or accept |

All arrays are sized by these constants. There is no `malloc`, no recursion,
no VLA, no `stdio` in `core/`. Exact memory use is `sizeof(fl_sender_t)` and
`sizeof(fl_receiver_t)`; the harness prints both (`docs/MEMORY.md`).

### 6.2 Per-stream state (sender)

```
latest    : {valid, seq, gen_time, payload}   the newest waiting snapshot
inflight  : {valid, seq, sent_time}           the one frame awaiting ACK (immutable copy)
last_acked_seq, last_acked_gen : receiver knowledge from ACKs (never regress)
max_sent_seq, max_sent_gen     : highest seq/gen ever handed to the transport
last_tx_time                   : for starvation accounting (FL_TIME_NONE until first tx)
next_seq
```

Rules:
* `publish_state` overwrites `latest` (superseding any older *waiting*
  snapshot; counted `state_superseded`). It never touches `inflight`.
* At most one in-flight frame per stream. A stream is **eligible** when
  `latest.valid && latest.seq > last_acked_seq && !inflight.valid`.
* Sending copies `latest` into the frame and records `inflight = {seq, now}`.
  The frame bytes are produced once; nothing later mutates them.
* `inflight` is cleared when (a) an ACK_STATE arrives with
  `applied_seq >= inflight.seq`, or (b) `now - sent_time >= ack_timeout_ms`
  at a step (`state_ack_timeouts++`). In case (b), if `latest.seq` is still
  `> last_acked_seq` the stream is simply eligible again (retry of the
  latest; state retries are unbounded because a newer snapshot bounds them).

### 6.3 Event slots (sender)

Each of `FL_EVENT_CAPACITY` slots: `{used, id, kind, code, gen_time,
deadline_abs, retention_abs, payload, attempts, in_flight, sent_time}`.

* `post_event(now, kind, code, deadline_rel, retention_rel, payload)`:
  validates `deadline_rel <= retention_rel <= FL_MAX_REL_MS`
  (else `FL_ERR_REL_RANGE`, no ID consumed), then assigns the next ID
  (`events_generated++`). If no slot is free: `events_rejected_full++`,
  returns `FL_ERR_EVENT_FULL`, the ID is reported to the host and burned.
  Otherwise the slot is filled, `events_admitted++`, returns `FL_OK`.
* An admitted event is **eligible** when `!in_flight && now < retention_abs`.
* Sending: `attempts++`, `in_flight = 1`, `sent_time = now`.
* ACK_EVENT validation, in order:
  1. `id == 0` or `id > events_generated` → **impossible** (never generated):
     `acks_impossible++`, ignored.
  2. no used slot has this ID → `acks_unmatched++` (already terminal or
     rejected at admission), ignored.
  3. used slot but `attempts == 0` → **impossible** (admitted but never
     transmitted; a matching allocated ID alone must not confirm anything):
     `acks_impossible++`, ignored.
  4. otherwise terminal `ACKED`, slot freed.
  Because IDs are never reused an ACK can never be applied to the wrong
  record, and because of rule 3 it can never confirm an unsent one.
* ACK_STATE validation: `applied_seq > max_sent_seq[stream]`, or
  `applied_gen_time > max_sent_gen[stream]`, or `applied_gen_time > now`
  → **impossible** (`acks_impossible++`, ignored). An ACK for a seq that was
  published but superseded before transmission cannot be distinguished
  without per-seq history and is accepted; it only ever leaves the sender's
  knowledge at or below the receiver's truth, which is the safe direction.
  The `interval_violations` check (§6.4) would expose a receiver that
  reported an unsent seq.
* Step housekeeping, in this order, before any scheduling decision:
  1. **Retention expiry:** every used slot with `now >= retention_abs`
     becomes terminal `RETENTION_EXPIRED` (even if in flight).
  2. **ACK timeout:** every remaining in-flight slot with
     `now - sent_time >= ack_timeout_ms` leaves flight; if
     `attempts >= max_attempts` it becomes terminal `RETRY_EXHAUSTED`.
  An event that would satisfy both in one step is `RETENTION_EXPIRED`.
* Terminal outcomes are returned to the host in the step/ACK result
  (bounded: at most `FL_EVENT_CAPACITY` per step, one per ACK).

### 6.4 Sender knowledge of the receiver

The only receiver knowledge the sender has is what ACKs carried:
`last_acked_seq/gen` per stream, and the set of ACKed event IDs (as terminal
outcomes). An ACK proves *past receipt and application under §7's rules*; it
does not prove the receiver's current state, because later frames may have
been applied whose ACKs were lost. The sender-side freshness estimate
`est_aoi(s) = now - last_acked_gen[s]` (or "unknown" if never ACKed) is
therefore a pessimistic proxy, and the candidate policy is only ever given
this proxy, never the receiver's true state or the channel trace.

**Interval statement (tested property, not a universal invariant).** Under
the assumptions (a) both ends share one session, (b) the receiver applies a
STATE frame only if its seq is greater than the current one (§7.1), and
(c) an ACK_STATE reports the receiver's applied seq at ACK time, the
receiver's true applied seq for stream `s` lies in
`[last_acked_seq[s], max_sent_seq[s]]` at every instant. The harness checks
this at every millisecond of every run against the receiver's real state
(`interval_violations` must be 0). It is a property of this simulation under
those assumptions; a different session model, a regressing receiver, or a
cumulative-ACK scheme would need its own proof and test.

**Structural separation.** Policy code lives in `core/fl_policy.c` and takes
only a read-only `fl_sched_view_t` built by the sender from its own ledger:
`now`, the policy parameters, the eligible event list (id, deadline_abs,
gen_time), the eligible stream list (id, `latest.gen_time`, `last_tx_time`,
`last_acked_gen`), and the round-robin cursor. It has no pointer to the
receiver, the channel trace, the workload, or the sender's non-eligible
records. `fl_policy.c` does not include any receiver or host header; the
Makefile compiles it as its own translation unit and a test asserts the view
struct is the only input.

### 6.5 Terminal outcomes (sender ledger) — exactly one per generated event

| Outcome | When |
|---|---|
| `REJECTED_FULL` | at generation, no free slot |
| `ACKED` | ACK_EVENT received while the slot is used |
| `RETRY_EXHAUSTED` | last permitted attempt timed out without ACK |
| `RETENTION_EXPIRED` | `now >= retention_abs` while still pending |
| `PENDING` (censored) | not terminal when the run ends; reported, never hidden |

Each terminal note carries `terminal_time` (the `now` at which the outcome
was decided; for `ACKED` that is the ACK arrival time, so
`confirmation_latency = terminal_time - gen_time` is distinct from the
receiver's first-delivery latency).

Deadline miss is **not** a sender terminal outcome: the sender keeps trying
after the deadline until retention or the retry limit ends it, because a late
occurrence still has ledger value. Whether delivery was on time is a
receiver-side fact (§7.3).

---

## 7. Receiver core

### 7.1 Per-stream state
`{valid, seq, gen_time, payload}`. A STATE frame is applied iff
`seq > current.seq` (or the stream is not yet valid). Otherwise it is counted
`rx_state_stale_dropped` and **not** applied. In both cases the receiver
answers `ACK_STATE{stream, applied_seq = current.seq, applied_gen_time}` —
i.e. the ACK reports what the receiver actually holds, so the ACK semantics
are "my applied seq is at least this". The receiver never regresses.

### 7.2 Event dedup window
`highest_id` (0 = none) and a `FL_RX_DEDUP_WINDOW`-bit bitmap where bit `k`
represents ID `highest_id - k` (bit 0 = `highest_id` itself).

For an incoming EVENT with `id`:
* `id > highest_id`: shift the bitmap by `id - highest_id` (bits that fall off
  the end are forgotten — that is the bounded eviction), set bit 0,
  `highest_id = id`, **deliver**.
* `highest_id - id < FL_RX_DEDUP_WINDOW` (in window): if the bit is set →
  duplicate: counted `rx_event_duplicate`, ACK with `FL_ACKF_DUPLICATE`, not
  delivered again. If clear → set it and **deliver** (reordered but new).
* otherwise (`highest_id - id >= FL_RX_DEDUP_WINDOW`, out of window): the
  receiver cannot know whether it already delivered this ID, so it **rejects**
  (`rx_event_out_of_window++`), does **not** deliver and does **not** ACK.
  Consequence: the sender keeps retrying until retention/retry limit and the
  event ends as `RETENTION_EXPIRED` or `RETRY_EXHAUSTED` — an explicit,
  counted failure, never a silent duplicate or silent loss.

**Declared dedup scope:** "no duplicate application event" is guaranteed for
IDs within `FL_RX_DEDUP_WINDOW` of the highest delivered ID. IDs older than
that are refused rather than guessed.

### 7.3 Deadline boundary
`on_time := rx_time <= deadline_abs` (inclusive). `rx_time == deadline_abs`
is on time; `deadline_abs + 1` is late. Late delivery is counted
`rx_event_late`, still delivered, still ACKed, and is **not** counted as
on-time success anywhere.

### 7.5 Bounded ACK queue and reverse service
Ingesting a frame never transmits. It **enqueues** an acknowledgement in the
receiver's fixed-size ACK queue, which the host drains at reverse-direction
transmit opportunities:
* `ack_state_pending[FL_MAX_STREAMS]` — one flag per stream. ACK_STATE is
  *replaceable*: if the flag is already set the new request coalesces
  (`rx_ack_state_coalesced++`). The ACK frame is built at transmit time from
  the receiver's current applied seq/gen, so it reports what the receiver
  holds at ACK time (§7.1).
* `ack_event_queue[FL_RX_ACK_QUEUE]` — FIFO ring of `(id, flags)`. ACK_EVENT
  is *not* replaceable. If the ring is full the new entry is dropped and
  counted `rx_ack_event_dropped` (explicit reverse-queue overflow; the sender
  will retry and the receiver will see a duplicate and try to ACK again).
* `fl_receiver_step(now)` emits **at most one** ACK frame: the head of the
  event ring if non-empty, else the next flagged stream in round-robin order,
  else nothing. This reverse order is fixed and identical for every policy.

### 7.4 Receiver ledger vs sender ledger
The receiver's per-ID delivery record and the sender's per-ID terminal outcome
are independent. Under ACK loss an event can be `delivered` at the receiver
while `PENDING`, `RETRY_EXHAUSTED` or `RETENTION_EXPIRED` at the sender. The
harness reports `delivered_but_unacked` explicitly. Recall and on-time rates
are computed from the **receiver** ledger over **all generated** IDs
(including `REJECTED_FULL` ones, which count as failures).

**Local accounting only.** Every counter above is local to the end that
observes it. The receiver is **not** told about sender-side rejections,
expiries or retry exhaustion in v0.1, and the sender is not told about
out-of-window rejections. Only the host, which can read both ledgers, sees
the gap. "Every loss eventually becomes visible to the receiver" is **not**
claimed; a receiver-visible loss-notification contract is *PLANNED* and would
need its own delivery assumptions and tests.

---

## 8. Host harness (HOST SIMULATION)

### 8.1 Simulation loop
`t` runs over integer milliseconds `0 .. run_ms - 1`. A **transmit
opportunity (slot)** exists in *each direction* at every `t` with
`t % slot_ms == 0`; slot index `k = t / slot_ms`. Order inside one
millisecond:

1. Apply workload actions with `time_ms == t`, in file order.
2. Deliver DATA frames whose arrival time is `t` to the receiver, in send
   order (`fl_receiver_ingest`, which only enqueues ACKs).
3. Deliver ACK frames whose arrival time is `t` to the sender, in send order.
4. If `t` is a slot: `fl_sender_step(t)` may produce one DATA frame, charged
   and routed by the DATA channel entry for slot `k`; then
   `fl_receiver_step(t)` may produce one ACK frame, charged and routed by
   the ACK channel entry for slot `k`.
5. Metrics integration (AoI, interval check) at every `t` after step 4.

Frames whose arrival time is `>= run_ms` are counted `in_transit_at_end`.
Data that arrives at a non-slot millisecond waits in the ACK queue until the
next reverse slot, so simultaneous and delayed arrivals are serialised by the
bounded reverse service rather than acknowledged instantly.

### 8.2 Matched channel trace
The trace is a CSV indexed by **slot and direction**:
`slot,data_lost,data_delay_ms,ack_lost,ack_delay_ms`. Every policy reads the
same entry for the same slot regardless of what it chose to send; unused
entries are simply unused. This is what makes runs matched. A per-transmission
random draw would let policies consume the randomness differently and is not
used anywhere. Delays are `>= 1 ms`; reordering emerges when
`delay(t1) > delay(t2) + (t2 - t1)`.

### 8.3 Link service model and budget (full duplex, finite both ways)
* Forward direction: one DATA frame per slot, at most `FL_MAX_FRAME_LEN`
  bytes, chosen by the policy.
* Reverse direction: one ACK frame per slot, taken from the bounded receiver
  ACK queue (§7.5) by a fixed rule that is the same for every policy.
* Both directions use the same `slot_ms`, the same trace, and the same queue
  capacities in every run of a scenario; the policy only changes which
  forward frame is chosen.
* Every transmitted frame (data or ACK, first attempt or retry, delivered or
  lost) is charged its bytes to `bytes_data_tx` / `bytes_ack_tx`. There are
  no other control frames in v0.1. Bytes are bytes; **nothing here is an
  energy measurement.**

### 8.4 Workload
CSV `time_ms,action,a,b,c` sorted by `time_ms` then file order:
* `STATE,stream,value` — publish an 8-byte payload whose first 4 bytes are the
  little-endian `int32 value` (rest zero).
* `RAISE,code,deadline_rel,retention_rel` and `CLEAR,code,deadline_rel,retention_rel`.
Same-time actions apply in file order, so same-time events get increasing IDs
in file order. The event's payload carries the code.

### 8.5 Metrics
Per run (`summary.csv` row):
* Sender ledger: `ev_generated, ev_admitted, ev_rejected_full, ev_acked,
  ev_retry_exhausted, ev_retention_expired, ev_pending_end`. Invariant:
  `generated = rejected_full + acked + retry_exhausted + retention_expired + pending_end`.
* Receiver ledger: `rx_ev_delivered, rx_ev_on_time, rx_ev_late,
  rx_ev_duplicate, rx_ev_out_of_window, rx_frames_rejected,
  rx_session_mismatch, rx_state_applied, rx_state_stale_dropped,
  rx_ack_event_dropped, rx_ack_state_coalesced`.
* Sender ACK validation: `acks_unmatched, acks_impossible`.
* `recall = rx_ev_delivered / ev_generated`, `on_time_rate = rx_ev_on_time / ev_generated`
  (denominator: **all** generated events, including rejected and undelivered).
* `delivered_but_unacked` = receiver-delivered IDs whose sender outcome is
  not `ACKED`.
* Delivered latency `rx_time - gen_time`: mean and max over delivered events
  (reported next to the failure counts, never alone).
* Per stream (`state.csv`): time-weighted mean AoI, peak AoI, `unknown_ms`,
  `over_threshold_ms`, published, superseded, sent, applied, stale-dropped.
* Link: `data_frames, ack_frames, bytes_data_tx, bytes_ack_tx,
  event_attempts, event_retries (= attempts - first attempts), state_tx,
  state_ack_timeouts, in_transit_at_end`.
* `interval_violations`: count of (t, stream) where the receiver's true
  applied seq was outside `[last_acked_seq, max_sent_seq]` (§6.4); must be 0.
* Confirmation latency `ack_time - gen_time` for `ACKED` events, reported
  separately from first-delivery latency.
* Memory: `sizeof_sender, sizeof_receiver` (bytes, this build's constants).
* Host runtime in ms, labelled as host runtime only.

### 8.6 AoI definition (continuous time, trapezoidal)
At the receiver, per stream, `AoI(t) = t - gen_time(current snapshot)` for
real-valued `t`. AoI is **undefined until the first applied snapshot**; that
interval is reported as `unknown_ms` (from `t = 0`, the time origin — not
from the first publish) and excluded from the AoI integral. From the first
apply, AoI is piecewise linear with slope 1 and drops at each apply.

The integral is the **continuous-time area** under that sawtooth: a segment of
length `n` starting at age `a0` contributes `n*a0 + n*n/2`. To keep exact
integer arithmetic the accumulator stores `2*area` in `uint64_t`
(`2*n*a0 + n*n`) and divides once at reporting time. Time-weighted mean AoI
is `area / (run_ms - first_apply_time)`. Peak AoI is the largest age reached
at the end of any segment (just before an apply, or at the end of the run).
Final age is `run_ms - gen_time(current)`.

**Hand fixture (unit test `test_aoi_fixture`):** samples applied with
(gen, rx) = (0,0), (2,3), (6,8), run end 10. Segments: [0,3) from age 0 →
area 4.5; [3,8) from age 1 → 17.5; [8,10) from age 2 → 6. Total area 28
(`2*area = 56`), mean 2.8, peak 6 (just before t=8), final age 4,
`unknown_ms = 0`. The test asserts these exact numbers.

**No warm-up exclusion** is applied in v0.1; `unknown_ms` is the warm-up and
is reported. `over_threshold_ms` is the continuous-time measure of
`{t : AoI(t) > aoi_threshold_ms}`, computed on the same segments: a segment
of length `n` starting at age `a0` contributes `n` if `a0 >= θ`, else
`max(0, a0 + n - θ)`. Mean, peak, final age and threshold time are therefore
all continuous-time quantities; **no discrete left-sample sums are used
anywhere** (those would give area 23 / mean 2.3 on the fixture, which is not
what is reported).

### 8.7 End of run
Pending events are `PENDING` (censored) and counted in the accounting
identity. Frames in transit are counted. Nothing is extrapolated.

### 8.8 Decision log
Every slot writes `slot,t,kind,reason,ref,attempt,n_elig_ev,n_elig_st` where
`kind ∈ {NONE, STATE, EVENT}`, `ref` = stream or event ID, and `reason` is one
of `IDLE, EVENT_EDF, EVENT_LATE, EVENT_LATE_DEMOTED, EVENT_URGENT,
EVENT_SLACK_OK, STATE_RR, STATE_FRESH, STATE_STARVATION, STATE_STALE,
FIFO_OLDEST`.

---

## 9. Policies

All policies share admission, retry, timeout, expiry, encoding and ACK
handling (§6). Only `fl_policy_select()` differs. Let `E` be eligible events
sorted by `(deadline_abs, id)` and `S` the eligible streams (§6.2).

### 9.0 Shared definitions
* `late(e)` iff `deadline_abs < now`.
* `remaining(e) = deadline_abs > now ? deadline_abs - now : 0`.
* **Late-event demotion** (`late_demote`, option available to *every* policy):
  when on, late events are removed from `E` and served only when the policy
  would otherwise go `IDLE` (reason `EVENT_LATE_DEMOTED`, EDF order among
  late events). When off, late events stay in `E` in EDF order (they have the
  earliest deadlines, so they are served first; reason `EVENT_LATE`). The
  option is applied by the same code for every policy so no policy quietly
  gets it alone.
* **Freshness state ranking** `pick_fresh(S)`: the stream with the largest
  sender-side estimate `est_aoi(s) = now - last_acked_gen[s]`
  (`NONE` = infinite), tie → lowest stream id.
* **Round-robin state ranking** `pick_rr(S)`: scan from `rr_next`, take the
  first eligible stream, `rr_next = chosen + 1 mod n_streams`.
* `starved(s)` iff `s ∈ S` and (`last_tx_time[s] == NONE` or
  `now - last_tx_time[s] >= state_starvation_ms`).
* **Starvation ranking** `pick_starved(S)`: among *starved* eligible streams,
  the one with the largest `now - last_tx_time` (`NONE` = infinite), tie →
  lowest stream id. This is intentionally not `pick_fresh`: a
  `STATE_STARVATION` decision must serve a stream that actually triggered
  the guard. (Regression case: `now=100`, A `last_tx=95, last_acked_gen=0`,
  B `last_tx=0, last_acked_gen=50`, threshold 30 → B must be chosen even
  though A has the larger `est_aoi`.)
* `stale(s)` iff `s ∈ S` and `est_aoi(s) >= state_stale_ms` (NONE counts).
* `E[i]` is **urgent** iff `remaining(E[i]) <= (i+1)*event_service_ms + slack_guard_ms`.
  The `(i+1)` term is the declared service assumption: one event per
  `event_service_ms`, served in EDF order, no loss.

### 9.1 `edf_rr` — named comparison (event EDF, latest-state round robin)
1. If `E` non-empty: send `E[0]` (reason `EVENT_EDF`, or `EVENT_LATE`).
2. Else if `S` non-empty: `pick_rr(S)` (reason `STATE_RR`).
3. Else demoted late events if any, else `IDLE`.

### 9.2 `fresh_nodefer` — ablation baseline
Identical to the candidate in state ranking, late-event treatment and tie
breakers; it never defers an eligible event.
1. If `E` non-empty: send `E[0]` (reason `EVENT_EDF`, or `EVENT_LATE`).
2. Else if `S` non-empty: `pick_fresh(S)` (reason `STATE_FRESH`).
3. Else demoted late events if any, else `IDLE`.

### 9.3 `fresh` — candidate (ablation: `fresh_nodefer` + permission to defer)
Parameters: `event_service_ms`, `slack_guard_ms`, `state_stale_ms`,
`state_starvation_ms`, `defer` (1 = candidate, 0 = must reproduce
`fresh_nodefer` exactly), `starvation_override` (0 for the candidate).
1. If `E` and `S` both non-empty and `defer == 1`:
   a. if any `E[i]` is urgent: `E[0]` — reason `EVENT_URGENT` (or `EVENT_LATE`);
   b. else if any `starved(s)`: `pick_starved(S)` — reason `STATE_STARVATION`;
   c. else if any `stale(s)`: `pick_fresh(S)` — reason `STATE_STALE`;
   d. else `E[0]` — reason `EVENT_SLACK_OK`.
2. Else if `E` non-empty: `E[0]` (reason `EVENT_EDF`, or `EVENT_LATE`).
3. Else if `S` non-empty: `pick_fresh(S)` (reason `STATE_FRESH`).
4. Else demoted late events if any, else `IDLE`.

The candidate therefore **obeys its event guard**: it only defers an event
whose declared slack allows it. This gives no deadline guarantee under
arbitrary loss — the guard assumes the declared service time and no loss, and
the `tight_deadline` scenario is designed to violate that assumption. Under
persistent event overload the candidate degenerates to `fresh_nodefer` only
while three or more events are *simultaneously eligible* (in-flight events
do not count); the pilot `overload` runs show that it still defers often
enough to cut mean AoI about five-fold at the cost of about one percentage
point of event recall (docs/REPORT.md). That trade-off is measured, not
hidden, and the reduced counterexample shows the mechanism. **No strict starvation bound is claimed:** the guard only acts when
no event is urgent, so the time between two transmissions of a stream is
bounded only under the assumption that urgent events do not occupy every
slot.

### 9.3b `fresh_so` — separate ablation: starvation override
Same as `fresh` with `starvation_override = 1`: step 1b is evaluated
**before** 1a, so a starving stream is served even when an event is urgent.
This is a different trade-off (state starvation limited to roughly
`state_starvation_ms` per stream while the link accepts frames, bought with
possible deadline misses), named and reported separately; it is *not* the
candidate. Even here the bound holds only for transmissions, not for
deliveries: a lost frame still leaves the receiver stale.

**Ablation test:** for every scenario and seed, `fresh` with `defer=0` must
produce a byte-identical `decisions.csv` to `fresh_nodefer`, which is a
separately written function. `fresh_nodefer` differs from `edf_rr` only in
step 2 (`pick_fresh` vs `pick_rr`); a test confirms their decisions agree on
every slot where `E` is non-empty.

### 9.4 `fifo` — secondary
One ordering key across kinds: events by `gen_time`, streams by the
`gen_time` of `latest`. Send the smallest key; ties → event before state,
then lower id / lower stream. Reason `FIFO_OLDEST`. Late demotion applies as
in §9.0.

### 9.5 Named presets used by the matrix
| Preset | family | defer | late_demote |
|---|---|---|---|
| `edf_rr` | edf_rr | – | 0 |
| `edf_rr_ld` | edf_rr | – | 1 |
| `fresh_nodefer` | fresh_nodefer | – | 0 |
| `fresh` (candidate) | fresh | 1 | 0 |
| `fresh_ld` | fresh | 1 | 1 |
| `fresh_so` | fresh + starvation_override | 1 | 0 |
| `fifo` | fifo | – | 0 |

Parameter values for `fresh` are chosen on tuning seeds only and are
reported as such. No numeric target was preregistered; results are reported
whether or not the candidate wins.

## 10. Scenarios (all HOST SIMULATION)

Each scenario = config + workload generator + trace generator, keyed by seed.
Every parameter in a scenario's `.cfg` (retry limit, ACK timeout, service
slot, capacities, candidate parameters) applies **identically to every
policy** run on that scenario; only the policy name differs between runs.

| Name | Purpose |
|---|---|
| `healthy_light` | light load, 1% loss: sanity, both should be near-perfect |
| `alarm_outage` | alarm raised/cleared inside an outage: semantics demo |
| `burst_loss` | Gilbert–Elliott bursty loss on data direction |
| `ack_loss` | reverse-direction loss: sender uncertainty, ledgers diverge |
| `reorder` | alternating delays: old STATE and old ACK arrive after newer |
| `overflow` | more events than `FL_EVENT_CAPACITY` during an outage: explicit rejection |
| `overload` | persistent event overload: state starvation vs guard |
| `tight_deadline` | short deadlines + loss: designed so the slack candidate loses |

### 10.1 Seed provenance (accurate history, no preregistration claimed)

* **Development / pilot seeds: `1, 2, 3, 101, 102, 103, 104, 105`.** These
  were used while building the generators (structural checks over all of
  them), while verifying the tools (seeds 101/102), and while inspecting the
  `alarm_outage` and `overflow` demos (seed 101). The decision to raise
  `max_attempts` for the two outage scenarios (§10.2) was taken **after**
  looking at seed-101 output. None of these seeds is therefore an untouched
  evaluation seed, and nothing about them was preregistered.
* **Configuration freeze.** After that change no scenario configuration or
  candidate parameter is modified. The candidate's four parameters
  (`event_service_ms=40`, `slack_guard_ms=100`, `state_stale_ms=1000`,
  `state_starvation_ms=2000`) were set from the link parameters (10 ms slot,
  20 ms one-way delay, 300 ms ACK timeout) before any comparison run; no
  tuning sweep was performed on any seed.
* **This first matrix is exploratory / pilot.** It is run on seeds
  `101..105` (development seeds by the history above) and is labelled
  exploratory/pilot everywhere it is reported. It is *not* a held-out
  evaluation and no preregistration is claimed. A separate held-out study,
  with seeds chosen after this milestone and audited for prior use, is a
  later milestone (*PLANNED*).
* The `alarm_outage` scenario's stream 0 and its two events do not depend on
  the seed at all (only stream 1's jitter and values do), so the demo
  walkthrough is the same story on any seed; it is still reported from a
  held-out seed for consistency.

### 10.2 Retry budget limitation (recorded failure)

With the common configuration `ack_timeout_ms=300, max_attempts=8`, an
event's effective lifetime is at most `1 + 7 × 300 ms = 2.1 s` after its
first transmission, regardless of `retention_rel`. In the first
`alarm_outage` pilot run (seed 101, outage 9–15 s) both events ended
`RETRY_EXHAUSTED` at 12 400 ms and 14 400 ms, inside the outage, with
`rx_ev_delivered = 0`; the declared 17 s retention never mattered. The same
would happen in `overflow` (7 s outage). This is a genuine limitation of a
fixed retry count without backoff: **retention only matters if
`max_attempts × ack_timeout_ms` covers the outage.** The two outage scenarios
therefore use `max_attempts=40` (12 s of retries, longer than the longest
scripted outage, still capped by retention), applied identically to all
policies. All other scenarios keep `max_attempts=8`. A back-off or
time-based retry budget is *PLANNED*.

## 11. Determinism
Given identical (workload, trace, config, policy) the harness produces
byte-identical output files. The tools are seeded (`random.Random(seed)`) and
never read the clock. Host runtime is the only non-deterministic number and is
labelled as such.

---

## 12. Test plan (see `tests/`)
* Time: horizon, regress, equal-time, `gen + rel` at the boundary.
* Frames: round trip for all 4 types; truncated, oversize, bad magic, bad
  type, wrong session, length field mismatch.
* Events: immutable identity; generated = admitted + rejected; full-buffer
  rejection burns an ID; retry limit; retention expiry; retention vs deadline
  are distinct; same-time ordering; deadline ties; unmatched ACK; ACK after
  terminal; expiry beats exhaustion in the same step.
* State: latest waiting superseded; in-flight copy untouched by publish; ACK
  never regresses; reordered ACK; timeout then retry; seq exhaustion.
* Receiver: dedup in window; reordered-new in window; out-of-window rejection
  (never delivered, never ACKed); stale STATE dropped and ACK reports applied
  seq; session mismatch; deadline inclusive boundary.
* Reverse service: two frames arriving in the same millisecond produce two
  ACKs over two reverse slots (event ACK first); a delayed arrival waits for
  the next reverse slot; `FL_RX_ACK_QUEUE + 1` event arrivals before any
  reverse slot drop exactly one ACK (counted) and the sender later recovers
  it through a retry/duplicate; state ACK coalescing.
* ACK validation: ACK for an ID above `events_generated`; ACK for an admitted
  but never-sent event (must stay pending, counted impossible); ACK for a
  freed ID (unmatched); ACK_STATE with seq above `max_sent_seq` or gen above
  `max_sent_gen`/`now` (impossible, knowledge unchanged).
* Scheduling: hand-computed decision sequences for each policy on tiny
  traces, including the starvation guard and urgency queue term, and the
  starvation-ranking regression (§9.0: B must be chosen, not A).
* Accounting: identity over a full simulated run; ledgers independent under
  ACK loss (`delivered_but_unacked > 0`).
* Determinism: the same run twice → identical bytes.
* Ablation: `fresh defer=0` ≡ `fresh_nodefer` decision traces on all scenarios.
* Interval: `interval_violations == 0` on all scenarios.
* **AoI hand fixture** (§8.6): area 28, mean 2.8, peak 6, final 4.
* **Lost-ACK ledger fixture** (`test_lost_ack_fixture`), driven directly
  through the core API with `ack_timeout_ms = 5`, `max_attempts >= 2`:
  event E generated at t=0 with `deadline_rel = 10`, `retention_rel = 20`;
  sent at t=1, received at t=2, its ACK is attempted at t=2 and lost; the
  retry is sent at t=6 (timeout 1+5), received at t=7 as a duplicate, the
  second ACK arrives at t=8. Frame sizes here are EVENT = 27 bytes and
  ACK_EVENT = 10 bytes (§5). Expected:
  * at cutoff t=5: receiver unique delivered 1, on time 1, duplicates 0;
    sender pending 1, acked 0; attempted bytes = 27 + 10 = 37 (the lost ACK
    is charged).
  * at cutoff t=8 (after the ACK): unique 1, duplicate 1, application effects
    1; sender acked 1, pending 0; attempted bytes = 2·27 + 2·10 = 74;
    first-delivery latency 2, confirmation latency 8.
  These are specifications the test asserts; they are not measured results.
* Tools: Python `unittest` for generators/aggregator determinism and the
  AoI aggregator on the same fixture.
