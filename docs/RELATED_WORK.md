# Related work and honest positioning

Freshness Lab does not introduce a new idea. Latest-value storage, expiry,
priorities, bounded queues with overflow signalling, and freshness (AoI)
optimisation all exist in shipped standards and published research. What this
repository contributes is a **small, independently evaluated, constrained
integration** of those ideas in fixed memory, with **inspectable failures**
(every loss, rejection, expiry and late delivery is a counted, named outcome).

Nothing below was copied from any of these sources; no code from them is
used. Custom benchmarks in this repository are **not** evidence of
outperforming any of them and do **not** reproduce their experiments.

**Confidence note.** The comparisons summarise these specifications from
general familiarity. Section and clause numbers were not re-verified against
the documents during this session; where a specific mechanism is named,
treat the clause-level detail as *[Medium confidence — verify against the
linked document]*.

| Source | Mechanism relevant here | How Freshness Lab relates |
|---|---|---|
| **MQTT 5.0** (OASIS) — https://docs.oasis-open.org/mqtt/mqtt/v5.0/mqtt-v5.0.html | Retained messages (latest value per topic); Message Expiry Interval; QoS 1/2 delivery with packet identifiers; session expiry. | STATE streams behave like a retained latest value; EVENT retention resembles a message expiry interval. MQTT is broker-based and has no per-message *deadline* distinct from expiry, and its duplicate detection is per packet identifier in a session, not a bounded ID window. We do not implement MQTT. |
| **DDS 1.4** (OMG) — https://www.omg.org/spec/DDS/1.4/PDF | QoS policies: HISTORY (KEEP_LAST depth / KEEP_ALL), LIFESPAN (sample expiry), RELIABILITY, RESOURCE_LIMITS (bounded), DEADLINE. | STATE ≈ KEEP_LAST depth 1; EVENT ≈ KEEP_ALL + RELIABLE + LIFESPAN with bounded RESOURCE_LIMITS. **DDS DEADLINE is a contract on the maximum period between updates** (with a missed-deadline status); it is *not* a source-age/AoI guarantee and we do not describe it as one. |
| **Micro XRCE-DDS** (eProsima) — https://micro-xrce-dds.docs.eprosima.com/en/latest/client.html | Client–agent architecture for MCUs; best-effort and reliable streams over bounded, statically sized buffers. | Same fixed-memory intent for the constrained side; our frame/transport model is far simpler (one link, no agent, no discovery). Not compatible. |
| **Zephyr zbus** — https://docs.zephyrproject.org/latest/services/zbus/index.html | On-device message bus whose channels hold the latest message; observers (listeners, subscribers, message-queue subscribers). | Channel latest-value semantics match our STATE stream idea, but zbus is intra-device; Freshness Lab is about a lossy link between two ends. |
| **WiFresh** — https://arxiv.org/abs/2012.14337 | Application-layer AoI-aware scheduling for Wi-Fi networks; prioritises fresh packets over stale queued ones. | Shares the motivation (freshness over backlog). Our latest-waiting replacement is the simplest form of that idea. We do **not** reproduce WiFresh's scheduler, its multi-node setting, or its hardware experiments. |
| **ACP+ on ESP32** — https://arxiv.org/abs/2108.03476 | Age Control Protocol: source-side rate control to minimise age, measured on real hardware. | Different lever: ACP+ adapts the *sending rate*; we schedule a fixed workload. No hardware here (HOST SIMULATION only). Not a reproduction. |
| **AoII** — https://arxiv.org/abs/1907.06604 | Age of Incorrect Information: penalises time during which the receiver's view is wrong, not merely old. | The alarm-during-outage demo is exactly a case where latest state is *fresh but incorrect about what happened*. We report AoI plus event recall instead of computing AoII; AoII is a natural next metric (*PLANNED*). |
| **CoAP Observe** — RFC 7641, https://www.rfc-editor.org/rfc/rfc7641.html | Observe relationships; notifications carry an Observe sequence number used to detect and drop reordered (older) notifications; Max-Age freshness. | Our receiver's "apply only if seq is newer" rule for STATE is the same discipline. RFC 7641 uses a 24-bit sequence with wraparound and a time-based freshness rule; we use a 32-bit session-scoped sequence with no wraparound (declared exhaustion). |
| **OPC UA Part 4 (Services)** — MonitoredItem model (added after review) | Bounded per-item queues (queue size, discard-oldest); a data-change value carries an **Overflow** bit in its status when the queue overflowed; event monitored items signal overflow with an event queue overflow event type (defined in the information model part). | This is established prior art for *explicit* loss/overflow accounting; our `REJECTED_FULL` / `rx_ack_event_dropped` counters follow the same principle of never dropping silently. We do not implement OPC UA and did not verify the exact clause and part numbers in this session *[Medium confidence — verify]*. |

## What is genuinely ours (and small)
* One code base, two semantics, one link model, one accounting identity that
  must hold on every run (`generated = rejected + acked + exhausted + expired + pending`).
* Sender knowledge is *only* last-ACKed metadata, and the policy module cannot
  see anything else (structural separation, §6.4 of the design).
* Every comparison uses a slot-indexed matched channel trace so that a
  policy's choices cannot change the channel it experiences.
* Negative results are kept: the `tight_deadline` scenario is built to make
  the candidate lose and the report says whether it did.
