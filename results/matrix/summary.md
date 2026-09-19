# HOST SIMULATION — exploratory/pilot matrix (development seeds 101..105; not a held-out evaluation)

All numbers on this page are **HOST SIMULATION** results from `build/flsim` on a desktop CPU (docs/DESIGN.md scope label). No microcontroller board has run this code.

* **Exploratory / pilot matrix, not a held-out evaluation.** The seeds used here (101..105) are *development* seeds: they were used while building the generators, verifying the tools and inspecting the `alarm_outage` / `overflow` demos, and the `max_attempts=40` change for those two scenarios was made after looking at seed-101 output (DESIGN.md S10.1, S10.2). No preregistration is claimed. A held-out study on seeds chosen after this milestone and audited for prior use is a later milestone (*PLANNED*).
* **Bytes are not energy.** `bytes_total_tx` counts the bytes handed to the transport in both directions, first attempts and retries, delivered or lost (S8.3). It is not an energy, power or radio-time measurement.
* No superiority claim is made. Results are reported whether or not the candidate `fresh` wins (S9.5); the `alarm_outage` scenario is a statement about data semantics, not scheduler quality (S1).
* Each cell is the mean over the pilot seeds with `[min, max]` across seeds in brackets (a single value when all seeds agree); `n` is the number of seeds. `recall` and `on_time_rate` use **all generated events** as denominator, including rejected and undelivered ones (S8.5).
* All policies in one scenario x seed share the same workload, configuration and matched channel trace (S8.2); only the forward-frame choice differs. The candidate's four parameters were fixed from the link parameters before any comparison run and no tuning sweep was performed on any seed (S10.1).

## Provenance

* `git_head`: f18d875212fa25b4e15055b34e26592583d29f77
* `git_dirty_tracked`: False
* `git_untracked_files`: 1524
* `flsim_sha256`: 86f9a2637bb90b1e3e83362bc8aeb101427db5d2ec3c7c80ffd153129c09225e
* `seeds`: 101, 102, 103, 104, 105
* `policies`: edf_rr, edf_rr_ld, fresh_nodefer, fresh, fresh_ld, fresh_so, fifo
* `scenarios`: healthy_light, alarm_outage, burst_loss, ack_loss, reorder, overflow, overload, tight_deadline

## Checks (from checks.json)

* `ablation_defer0_bytes`: pass over 40 entries
* `ablation_defer0_summary`: pass over 40 entries
* `accounting_identity`: pass over 280 entries
* `edf_rr_vs_fresh_nodefer_event_slots`: pass over 40 entries
* `interval_violations_zero`: pass over 280 entries
* `ledger_fixtures`: pass over 17 entries

## Scenario `healthy_light`

### Event ledger

| policy | n | recall | on_time_rate | rx_ev_late | ev_rejected_full | ev_retention_expired | ev_retry_exhausted | ev_pending_end | delivered_but_unacked | lat_mean_ms | event_retries |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `edf_rr` | 5 | 1.0000 | 1.0000 | 0 | 0 | 0 | 0 | 0 | 0 | 24.1834 [23.5000, 24.9170] | 0 |
| `edf_rr_ld` | 5 | 1.0000 | 1.0000 | 0 | 0 | 0 | 0 | 0 | 0 | 24.1834 [23.5000, 24.9170] | 0 |
| `fresh_nodefer` | 5 | 1.0000 | 1.0000 | 0 | 0 | 0 | 0 | 0 | 0 | 24.1834 [23.5000, 24.9170] | 0 |
| `fresh` | 5 | 1.0000 | 1.0000 | 0 | 0 | 0 | 0 | 0 | 0 | 24.1834 [23.5000, 24.9170] | 0 |
| `fresh_ld` | 5 | 1.0000 | 1.0000 | 0 | 0 | 0 | 0 | 0 | 0 | 24.1834 [23.5000, 24.9170] | 0 |
| `fresh_so` | 5 | 1.0000 | 1.0000 | 0 | 0 | 0 | 0 | 0 | 0 | 24.1834 [23.5000, 24.9170] | 0 |
| `fifo` | 5 | 1.0000 | 1.0000 | 0 | 0 | 0 | 0 | 0 | 0 | 24.5166 [23.5000, 25.7500] | 0 |

### State freshness and link bytes

| policy | n | aoi_mean_ms | aoi_mean_defined_ms | aoi_defined_streams | aoi_peak_ms | over_threshold_ms_sum | unknown_ms_sum | bytes_total_tx |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `edf_rr` | 5 | 279.6752 [275.6550, 284.9010] | 279.6752 [275.6550, 284.9010] | 4 | 825.8 [621, 920] | 0 | 838.0 [780, 900] | 9236.4 [9156, 9316] |
| `edf_rr_ld` | 5 | 279.6752 [275.6550, 284.9010] | 279.6752 [275.6550, 284.9010] | 4 | 825.8 [621, 920] | 0 | 838.0 [780, 900] | 9236.4 [9156, 9316] |
| `fresh_nodefer` | 5 | 279.6808 [275.6550, 284.8900] | 279.6808 [275.6550, 284.8900] | 4 | 823.8 [621, 920] | 0 | 838.0 [780, 900] | 9236.4 [9156, 9316] |
| `fresh` | 5 | 279.6808 [275.6550, 284.8900] | 279.6808 [275.6550, 284.8900] | 4 | 823.8 [621, 920] | 0 | 838.0 [780, 900] | 9236.4 [9156, 9316] |
| `fresh_ld` | 5 | 279.6808 [275.6550, 284.8900] | 279.6808 [275.6550, 284.8900] | 4 | 823.8 [621, 920] | 0 | 838.0 [780, 900] | 9236.4 [9156, 9316] |
| `fresh_so` | 5 | 279.6808 [275.6550, 284.8900] | 279.6808 [275.6550, 284.8900] | 4 | 823.8 [621, 920] | 0 | 838.0 [780, 900] | 9236.4 [9156, 9316] |
| `fifo` | 5 | 279.6644 [275.6550, 284.8900] | 279.6644 [275.6550, 284.8900] | 4 | 823.8 [621, 920] | 0 | 838.0 [780, 900] | 9236.4 [9156, 9316] |

## Scenario `alarm_outage`

### Event ledger

| policy | n | recall | on_time_rate | rx_ev_late | ev_rejected_full | ev_retention_expired | ev_retry_exhausted | ev_pending_end | delivered_but_unacked | lat_mean_ms | event_retries |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `edf_rr` | 5 | 1.0000 | 0.0000 | 2 | 0 | 0 | 0 | 0 | 0 | 4070.0000 | 27 |
| `edf_rr_ld` | 5 | 1.0000 | 0.0000 | 2 | 0 | 0 | 0 | 0 | 0 | 4070.0000 | 27 |
| `fresh_nodefer` | 5 | 1.0000 | 0.0000 | 2 | 0 | 0 | 0 | 0 | 0 | 4070.0000 | 27 |
| `fresh` | 5 | 1.0000 | 0.0000 | 2 | 0 | 0 | 0 | 0 | 0 | 4076.0000 [4075.0000, 4080.0000] | 27 |
| `fresh_ld` | 5 | 1.0000 | 0.0000 | 2 | 0 | 0 | 0 | 0 | 0 | 4076.0000 [4075.0000, 4080.0000] | 27 |
| `fresh_so` | 5 | 1.0000 | 0.0000 | 2 | 0 | 0 | 0 | 0 | 0 | 4076.0000 [4075.0000, 4080.0000] | 27 |
| `fifo` | 5 | 1.0000 | 0.0000 | 2 | 0 | 0 | 0 | 0 | 0 | 4070.0000 | 27 |

### State freshness and link bytes

| policy | n | aoi_mean_ms | aoi_mean_defined_ms | aoi_defined_streams | aoi_peak_ms | over_threshold_ms_sum | unknown_ms_sum | bytes_total_tx |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `edf_rr` | 5 | 1072.5902 [1049.5430, 1081.9690] | 1072.5902 [1049.5430, 1081.9690] | 2 | 7032.0 [7030, 7040] | 9511.6 [9275, 9607] | 298.0 [250, 340] | 4285.0 [4275, 4311] |
| `edf_rr_ld` | 5 | 1072.5902 [1049.5430, 1081.9690] | 1072.5902 [1049.5430, 1081.9690] | 2 | 7032.0 [7030, 7040] | 9511.6 [9275, 9607] | 298.0 [250, 340] | 4285.0 [4275, 4311] |
| `fresh_nodefer` | 5 | 1072.5568 [1049.3760, 1081.9690] | 1072.5568 [1049.3760, 1081.9690] | 2 | 7030 | 9511.6 [9275, 9607] | 298.0 [250, 340] | 4285.0 [4275, 4311] |
| `fresh` | 5 | 1071.1896 [1047.2090, 1080.8020] | 1071.1896 [1047.2090, 1080.8020] | 2 | 7020 | 9499.6 [9255, 9597] | 298.0 [250, 340] | 4285.0 [4275, 4311] |
| `fresh_ld` | 5 | 1071.1896 [1047.2090, 1080.8020] | 1071.1896 [1047.2090, 1080.8020] | 2 | 7020 | 9499.6 [9255, 9597] | 298.0 [250, 340] | 4285.0 [4275, 4311] |
| `fresh_so` | 5 | 1071.1896 [1047.2090, 1080.8020] | 1071.1896 [1047.2090, 1080.8020] | 2 | 7020 | 9499.6 [9255, 9597] | 298.0 [250, 340] | 4285.0 [4275, 4311] |
| `fifo` | 5 | 1072.5902 [1049.5430, 1081.9690] | 1072.5902 [1049.5430, 1081.9690] | 2 | 7032.0 [7030, 7040] | 9511.6 [9275, 9607] | 298.0 [250, 340] | 4285.0 [4275, 4311] |

## Scenario `burst_loss`

### Event ledger

| policy | n | recall | on_time_rate | rx_ev_late | ev_rejected_full | ev_retention_expired | ev_retry_exhausted | ev_pending_end | delivered_but_unacked | lat_mean_ms | event_retries |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `edf_rr` | 5 | 1.0000 | 1.0000 | 0 | 0 | 0 | 0 | 0 | 0 | 123.2500 [69.0000, 189.7000] | 7.2 [3, 11] |
| `edf_rr_ld` | 5 | 1.0000 | 1.0000 | 0 | 0 | 0 | 0 | 0 | 0 | 123.2500 [69.0000, 189.7000] | 7.2 [3, 11] |
| `fresh_nodefer` | 5 | 1.0000 | 1.0000 | 0 | 0 | 0 | 0 | 0 | 0 | 123.2500 [69.0000, 189.7000] | 7.2 [3, 11] |
| `fresh` | 5 | 1.0000 | 1.0000 | 0 | 0 | 0 | 0 | 0 | 0 | 123.2500 [69.0000, 189.7000] | 7.2 [3, 11] |
| `fresh_ld` | 5 | 1.0000 | 1.0000 | 0 | 0 | 0 | 0 | 0 | 0 | 123.2500 [69.0000, 189.7000] | 7.2 [3, 11] |
| `fresh_so` | 5 | 1.0000 | 1.0000 | 0 | 0 | 0 | 0 | 0 | 0 | 123.2500 [69.0000, 189.7000] | 7.2 [3, 11] |
| `fifo` | 5 | 1.0000 | 1.0000 | 0 | 0 | 0 | 0 | 0 | 0 | 120.6500 [69.0000, 189.7000] | 7.0 [3, 11] |

### State freshness and link bytes

| policy | n | aoi_mean_ms | aoi_mean_defined_ms | aoi_defined_streams | aoi_peak_ms | over_threshold_ms_sum | unknown_ms_sum | bytes_total_tx |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `edf_rr` | 5 | 349.3538 [325.2340, 371.0050] | 349.3538 [325.2340, 371.0050] | 4 | 1447.4 [1196, 1646] | 0 | 898.0 [830, 1080] | 10459.6 [10211, 10669] |
| `edf_rr_ld` | 5 | 349.3538 [325.2340, 371.0050] | 349.3538 [325.2340, 371.0050] | 4 | 1447.4 [1196, 1646] | 0 | 898.0 [830, 1080] | 10459.6 [10211, 10669] |
| `fresh_nodefer` | 5 | 349.3718 [325.2340, 371.0020] | 349.3718 [325.2340, 371.0020] | 4 | 1447.4 [1196, 1646] | 0 | 898.0 [830, 1080] | 10452.4 [10211, 10669] |
| `fresh` | 5 | 349.3718 [325.2340, 371.0020] | 349.3718 [325.2340, 371.0020] | 4 | 1447.4 [1196, 1646] | 0 | 898.0 [830, 1080] | 10452.4 [10211, 10669] |
| `fresh_ld` | 5 | 349.3718 [325.2340, 371.0020] | 349.3718 [325.2340, 371.0020] | 4 | 1447.4 [1196, 1646] | 0 | 898.0 [830, 1080] | 10452.4 [10211, 10669] |
| `fresh_so` | 5 | 349.3718 [325.2340, 371.0020] | 349.3718 [325.2340, 371.0020] | 4 | 1447.4 [1196, 1646] | 0 | 898.0 [830, 1080] | 10452.4 [10211, 10669] |
| `fifo` | 5 | 349.8200 [325.2340, 371.0020] | 349.8200 [325.2340, 371.0020] | 4 | 1447.4 [1196, 1646] | 0 | 898.0 [830, 1080] | 10451.4 [10211, 10669] |

## Scenario `ack_loss`

### Event ledger

| policy | n | recall | on_time_rate | rx_ev_late | ev_rejected_full | ev_retention_expired | ev_retry_exhausted | ev_pending_end | delivered_but_unacked | lat_mean_ms | event_retries |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `edf_rr` | 5 | 1.0000 | 1.0000 | 0 | 0 | 0 | 0 | 0 | 0 | 24.1834 [23.5000, 24.9170] | 7.0 [5, 10] |
| `edf_rr_ld` | 5 | 1.0000 | 1.0000 | 0 | 0 | 0 | 0 | 0 | 0 | 24.1834 [23.5000, 24.9170] | 7.0 [5, 10] |
| `fresh_nodefer` | 5 | 1.0000 | 1.0000 | 0 | 0 | 0 | 0 | 0 | 0 | 24.1834 [23.5000, 24.9170] | 7.0 [5, 10] |
| `fresh` | 5 | 1.0000 | 1.0000 | 0 | 0 | 0 | 0 | 0 | 0 | 24.3502 [23.5000, 24.9170] | 6.8 [5, 10] |
| `fresh_ld` | 5 | 1.0000 | 1.0000 | 0 | 0 | 0 | 0 | 0 | 0 | 24.3502 [23.5000, 24.9170] | 6.8 [5, 10] |
| `fresh_so` | 5 | 1.0000 | 1.0000 | 0 | 0 | 0 | 0 | 0 | 0 | 24.3502 [23.5000, 24.9170] | 6.8 [5, 10] |
| `fifo` | 5 | 1.0000 | 1.0000 | 0 | 0 | 0 | 0 | 0 | 0 | 25.0168 [23.5000, 27.4170] | 8.2 [5, 14] |

### State freshness and link bytes

| policy | n | aoi_mean_ms | aoi_mean_defined_ms | aoi_defined_streams | aoi_peak_ms | over_threshold_ms_sum | unknown_ms_sum | bytes_total_tx |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `edf_rr` | 5 | 298.5628 [292.8210, 301.4520] | 298.5628 [292.8210, 301.4520] | 4 | 862.0 [754, 938] | 0 | 838.0 [780, 900] | 12913.0 [12714, 13259] |
| `edf_rr_ld` | 5 | 298.5628 [292.8210, 301.4520] | 298.5628 [292.8210, 301.4520] | 4 | 862.0 [754, 938] | 0 | 838.0 [780, 900] | 12913.0 [12714, 13259] |
| `fresh_nodefer` | 5 | 298.7458 [293.7230, 303.9110] | 298.7458 [293.7230, 303.9110] | 4 | 854.8 [754, 928] | 0 | 838.0 [780, 900] | 12930.2 [12786, 13187] |
| `fresh` | 5 | 298.7368 [293.6780, 303.9110] | 298.7368 [293.6780, 303.9110] | 4 | 854.8 [754, 928] | 0 | 838.0 [780, 900] | 12930.0 [12785, 13187] |
| `fresh_ld` | 5 | 298.7368 [293.6780, 303.9110] | 298.7368 [293.6780, 303.9110] | 4 | 854.8 [754, 928] | 0 | 838.0 [780, 900] | 12930.0 [12785, 13187] |
| `fresh_so` | 5 | 298.7368 [293.6780, 303.9110] | 298.7368 [293.6780, 303.9110] | 4 | 854.8 [754, 928] | 0 | 838.0 [780, 900] | 12930.0 [12785, 13187] |
| `fifo` | 5 | 298.3002 [293.6780, 302.6380] | 298.3002 [293.6780, 302.6380] | 4 | 852.8 [754, 928] | 0 | 838.0 [780, 900] | 12996.2 [12785, 13187] |

## Scenario `reorder`

### Event ledger

| policy | n | recall | on_time_rate | rx_ev_late | ev_rejected_full | ev_retention_expired | ev_retry_exhausted | ev_pending_end | delivered_but_unacked | lat_mean_ms | event_retries |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `edf_rr` | 5 | 1.0000 | 1.0000 | 0 | 0 | 0 | 0 | 0 | 0 | 75.5998 [53.0830, 115.0000] | 2.8 [1, 4] |
| `edf_rr_ld` | 5 | 1.0000 | 1.0000 | 0 | 0 | 0 | 0 | 0 | 0 | 75.5998 [53.0830, 115.0000] | 2.8 [1, 4] |
| `fresh_nodefer` | 5 | 1.0000 | 1.0000 | 0 | 0 | 0 | 0 | 0 | 0 | 75.5998 [53.0830, 115.0000] | 2.8 [1, 4] |
| `fresh` | 5 | 1.0000 | 1.0000 | 0 | 0 | 0 | 0 | 0 | 0 | 75.5998 [53.0830, 115.0000] | 2.8 [1, 4] |
| `fresh_ld` | 5 | 1.0000 | 1.0000 | 0 | 0 | 0 | 0 | 0 | 0 | 75.5998 [53.0830, 115.0000] | 2.8 [1, 4] |
| `fresh_so` | 5 | 1.0000 | 1.0000 | 0 | 0 | 0 | 0 | 0 | 0 | 75.5998 [53.0830, 115.0000] | 2.8 [1, 4] |
| `fifo` | 5 | 1.0000 | 1.0000 | 0 | 0 | 0 | 0 | 0 | 0 | 80.9334 [70.5830, 104.1670] | 2.8 [1, 4] |

### State freshness and link bytes

| policy | n | aoi_mean_ms | aoi_mean_defined_ms | aoi_defined_streams | aoi_peak_ms | over_threshold_ms_sum | unknown_ms_sum | bytes_total_tx |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `edf_rr` | 5 | 217.0678 [210.1090, 223.7970] | 217.0678 [210.1090, 223.7970] | 4 | 739.2 [638, 878] | 0 | 726.0 [440, 1250] | 21608.4 [21479, 21780] |
| `edf_rr_ld` | 5 | 217.0678 [210.1090, 223.7970] | 217.0678 [210.1090, 223.7970] | 4 | 739.2 [638, 878] | 0 | 726.0 [440, 1250] | 21608.4 [21479, 21780] |
| `fresh_nodefer` | 5 | 215.5372 [208.9580, 222.3340] | 215.5372 [208.9580, 222.3340] | 4 | 742.6 [651, 839] | 0 | 726.0 [440, 1250] | 21621.2 [21479, 21758] |
| `fresh` | 5 | 215.5372 [208.9580, 222.3340] | 215.5372 [208.9580, 222.3340] | 4 | 742.6 [651, 839] | 0 | 726.0 [440, 1250] | 21621.2 [21479, 21758] |
| `fresh_ld` | 5 | 215.5372 [208.9580, 222.3340] | 215.5372 [208.9580, 222.3340] | 4 | 742.6 [651, 839] | 0 | 726.0 [440, 1250] | 21621.2 [21479, 21758] |
| `fresh_so` | 5 | 215.5372 [208.9580, 222.3340] | 215.5372 [208.9580, 222.3340] | 4 | 742.6 [651, 839] | 0 | 726.0 [440, 1250] | 21621.2 [21479, 21758] |
| `fifo` | 5 | 215.6702 [210.1810, 221.3120] | 215.6702 [210.1810, 221.3120] | 4 | 742.6 [651, 839] | 0 | 726.0 [440, 1250] | 21606.8 [21479, 21758] |

## Scenario `overflow`

### Event ledger

| policy | n | recall | on_time_rate | rx_ev_late | ev_rejected_full | ev_retention_expired | ev_retry_exhausted | ev_pending_end | delivered_but_unacked | lat_mean_ms | event_retries |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `edf_rr` | 5 | 0.7000 | 0.5000 | 8 | 12 | 0 | 0 | 0 | 0 | 1925.4288 [1918.9290, 1930.0000] | 177.8 [177, 179] |
| `edf_rr_ld` | 5 | 0.7000 | 0.5000 | 8 | 12 | 0 | 0 | 0 | 0 | 1925.7144 [1918.9290, 1930.7140] | 177.8 [177, 179] |
| `fresh_nodefer` | 5 | 0.7000 | 0.5000 | 8 | 12 | 0 | 0 | 0 | 0 | 1925.4288 [1918.9290, 1930.0000] | 177.8 [177, 179] |
| `fresh` | 5 | 0.7000 | 0.5000 | 8 | 12 | 0 | 0 | 0 | 0 | 1925.7144 [1919.2860, 1930.3570] | 177.8 [177, 179] |
| `fresh_ld` | 5 | 0.7000 | 0.5000 | 8 | 12 | 0 | 0 | 0 | 0 | 1926.0000 [1919.2860, 1930.7140] | 177.8 [177, 179] |
| `fresh_so` | 5 | 0.7000 | 0.5000 | 8 | 12 | 0 | 0 | 0 | 0 | 1925.7144 [1919.2860, 1930.3570] | 177.8 [177, 179] |
| `fifo` | 5 | 0.7000 | 0.5000 | 8 | 12 | 0 | 0 | 0 | 0 | 1926.5712 [1920.3570, 1931.0710] | 177.8 [177, 179] |

### State freshness and link bytes

| policy | n | aoi_mean_ms | aoi_mean_defined_ms | aoi_defined_streams | aoi_peak_ms | over_threshold_ms_sum | unknown_ms_sum | bytes_total_tx |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `edf_rr` | 5 | 1138.0296 [1121.1740, 1171.2550] | 1138.0296 [1121.1740, 1171.2550] | 4 | 7574.6 [7457, 7767] | 21728.8 [21468, 22238] | 838.0 [780, 900] | 14741.0 [14706, 14830] |
| `edf_rr_ld` | 5 | 1137.9954 [1121.1740, 1171.2550] | 1137.9954 [1121.1740, 1171.2550] | 4 | 7574.6 [7457, 7767] | 21728.8 [21468, 22238] | 838.0 [780, 900] | 14741.0 [14706, 14830] |
| `fresh_nodefer` | 5 | 1137.9212 [1121.1740, 1171.2550] | 1137.9212 [1121.1740, 1171.2550] | 4 | 7574.6 [7457, 7767] | 21726.8 [21468, 22238] | 838.0 [780, 900] | 14741.0 [14706, 14830] |
| `fresh` | 5 | 1137.4422 [1119.9940, 1170.6710] | 1137.4422 [1119.9940, 1170.6710] | 4 | 7570.6 [7447, 7757] | 21718.8 [21448, 22228] | 838.0 [780, 900] | 14741.0 [14706, 14830] |
| `fresh_ld` | 5 | 1137.4080 [1119.9940, 1170.6710] | 1137.4080 [1119.9940, 1170.6710] | 4 | 7570.6 [7447, 7757] | 21718.8 [21448, 22228] | 838.0 [780, 900] | 14741.0 [14706, 14830] |
| `fresh_so` | 5 | 1137.4422 [1119.9940, 1170.6710] | 1137.4422 [1119.9940, 1170.6710] | 4 | 7570.6 [7447, 7757] | 21718.8 [21448, 22228] | 838.0 [780, 900] | 14741.0 [14706, 14830] |
| `fifo` | 5 | 1136.8922 [1118.8100, 1170.5840] | 1136.8922 [1118.8100, 1170.5840] | 4 | 7564.6 [7427, 7747] | 21710.8 [21428, 22228] | 838.0 [780, 900] | 14741.0 [14706, 14830] |

## Scenario `overload`

### Event ledger

| policy | n | recall | on_time_rate | rx_ev_late | ev_rejected_full | ev_retention_expired | ev_retry_exhausted | ev_pending_end | delivered_but_unacked | lat_mean_ms | event_retries |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `edf_rr` | 5 | 0.7822 [0.7808, 0.7848] | 0.7741 [0.7728, 0.7764] | 20.4 [17, 22] | 544.2 [538, 548] | 1.0 [0, 2] | 0 | 0 | 0.8 [0, 2] | 52.7934 [52.3930, 53.5880] | 47.2 [42, 51] |
| `edf_rr_ld` | 5 | 0.7695 [0.7500, 0.7836] | 0.7686 [0.7484, 0.7836] | 2.4 [0, 4] | 558.4 [522, 609] | 40.2 [37, 44] | 0 | 0 | 22.4 [18, 27] | 37.0168 [35.0130, 37.6020] | 12.6 [6, 20] |
| `fresh_nodefer` | 5 | 0.7822 [0.7808, 0.7848] | 0.7741 [0.7728, 0.7764] | 20.4 [17, 22] | 544.2 [538, 548] | 1.0 [0, 2] | 0 | 0 | 0.8 [0, 2] | 52.7934 [52.3930, 53.5880] | 47.2 [42, 51] |
| `fresh` | 5 | 0.7701 [0.7668, 0.7760] | 0.7622 [0.7592, 0.7676] | 19.8 [17, 21] | 574.6 [560, 583] | 1.0 [0, 2] | 0 | 0 | 0.8 [0, 2] | 54.0708 [53.7360, 54.4910] | 46.4 [42, 49] |
| `fresh_ld` | 5 | 0.7509 [0.7396, 0.7608] | 0.7502 [0.7388, 0.7608] | 1.8 [0, 4] | 605.4 [580, 636] | 39.4 [37, 43] | 0 | 0 | 22.0 [19, 26] | 39.1422 [37.3300, 40.3200] | 12.8 [8, 21] |
| `fresh_so` | 5 | 0.7640 [0.7608, 0.7680] | 0.7560 [0.7520, 0.7596] | 20.0 [17, 22] | 589.8 [580, 597] | 1.0 [0, 2] | 0 | 0 | 0.8 [0, 2] | 54.7516 [54.2350, 55.4700] | 46.2 [41, 49] |
| `fifo` | 5 | 0.6581 [0.6568, 0.6604] | 0.6514 [0.6504, 0.6532] | 16.8 [15, 18] | 854.6 [849, 858] | 1.0 [0, 2] | 0 | 0 | 0.8 [0, 2] | 68.3278 [67.5380, 69.2420] | 39.4 [35, 43] |

### State freshness and link bytes

| policy | n | aoi_mean_ms | aoi_mean_defined_ms | aoi_defined_streams | aoi_peak_ms | over_threshold_ms_sum | unknown_ms_sum | bytes_total_tx |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `edf_rr` | 5 | 6232.4672 [4405.8490, 6895.3030] | 6232.4672 [4405.8490, 6895.3030] | 4 | 19330.0 [15555, 20296] | 70623.6 [63402, 72907] | 470.0 [430, 490] | 79777.4 [79722, 79843] |
| `edf_rr_ld` | 5 | 1596.8454 [916.6510, 2526.5850] | 1596.8454 [916.6510, 2526.5850] | 4 | 6879.0 [4998, 8566] | 35483.6 [18367, 55868] | 470.0 [430, 490] | 79646.2 [79519, 79703] |
| `fresh_nodefer` | 5 | 6232.4672 [4405.8490, 6895.3030] | 6232.4672 [4405.8490, 6895.3030] | 4 | 19330.0 [15555, 20296] | 70623.6 [63402, 72907] | 470.0 [430, 490] | 79777.4 [79722, 79843] |
| `fresh` | 5 | 1210.2172 [951.6270, 1641.4270] | 1210.2172 [951.6270, 1641.4270] | 4 | 5438.2 [4564, 7253] | 26864.0 [17134, 40388] | 470.0 [430, 490] | 79736.6 [79651, 79820] |
| `fresh_ld` | 5 | 567.2768 [472.6300, 610.5920] | 567.2768 [472.6300, 610.5920] | 4 | 2973.0 [2091, 4272] | 3856.2 [170, 8346] | 470.0 [430, 490] | 79561.0 [79411, 79636] |
| `fresh_so` | 5 | 716.1384 [677.7880, 754.1010] | 716.1384 [677.7880, 754.1010] | 4 | 2272.0 [2224, 2297] | 2484.6 [1973, 3271] | 470.0 [430, 490] | 79707.6 [79570, 79799] |
| `fifo` | 5 | 181.6080 [180.8360, 183.5950] | 181.6080 [180.8360, 183.5950] | 4 | 635.0 [626, 648] | 0 | 470.0 [430, 490] | 79358.4 [79258, 79440] |

## Scenario `tight_deadline`

### Event ledger

| policy | n | recall | on_time_rate | rx_ev_late | ev_rejected_full | ev_retention_expired | ev_retry_exhausted | ev_pending_end | delivered_but_unacked | lat_mean_ms | event_retries |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `edf_rr` | 5 | 0.9947 [0.9737, 1.0000] | 0.9368 [0.9211, 0.9737] | 2.2 [1, 3] | 0 | 0 | 0 | 0.2 [0, 1] | 0 | 120.5300 [94.7300, 148.9470] | 17.2 [14, 20] |
| `edf_rr_ld` | 5 | 0.9947 [0.9737, 1.0000] | 0.9368 [0.9211, 0.9737] | 2.2 [1, 3] | 0 | 0 | 0 | 0.2 [0, 1] | 0 | 122.4276 [95.2700, 148.9470] | 17.4 [14, 21] |
| `fresh_nodefer` | 5 | 0.9947 [0.9737, 1.0000] | 0.9368 [0.9211, 0.9737] | 2.2 [1, 3] | 0 | 0 | 0 | 0.2 [0, 1] | 0 | 120.5300 [94.7300, 148.9470] | 17.2 [14, 20] |
| `fresh` | 5 | 0.9947 [0.9737, 1.0000] | 0.9474 [0.8947, 0.9737] | 1.8 [1, 4] | 0 | 0 | 0 | 0.2 [0, 1] | 0 | 108.3042 [81.7570, 167.6320] | 13.8 [8, 18] |
| `fresh_ld` | 5 | 0.9947 [0.9737, 1.0000] | 0.9474 [0.8947, 0.9737] | 1.8 [1, 4] | 0 | 0 | 0 | 0.2 [0, 1] | 0 | 108.4094 [81.7570, 167.8950] | 14.2 [8, 19] |
| `fresh_so` | 5 | 0.9947 [0.9737, 1.0000] | 0.9474 [0.8947, 0.9737] | 1.8 [1, 4] | 0 | 0 | 0 | 0.2 [0, 1] | 0 | 108.3042 [81.7570, 167.6320] | 13.8 [8, 18] |
| `fifo` | 5 | 1.0000 | 0.9632 [0.9474, 1.0000] | 1.4 [0, 2] | 0 | 0 | 0 | 0 | 0 | 115.0580 [58.6840, 147.8950] | 15.4 [7, 27] |

### State freshness and link bytes

| policy | n | aoi_mean_ms | aoi_mean_defined_ms | aoi_defined_streams | aoi_peak_ms | over_threshold_ms_sum | unknown_ms_sum | bytes_total_tx |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `edf_rr` | 5 | 509.7070 [494.1330, 520.3520] | 509.7070 [494.1330, 520.3520] | 8 | 2520.0 [2244, 2839] | 1383.6 [622, 2259] | 1174.0 [860, 1500] | 42040.8 [41466, 42728] |
| `edf_rr_ld` | 5 | 509.4960 [494.1330, 519.4430] | 509.4960 [494.1330, 519.4430] | 8 | 2520.0 [2244, 2839] | 1383.6 [622, 2259] | 1174.0 [860, 1500] | 42063.4 [41507, 42728] |
| `fresh_nodefer` | 5 | 509.8164 [502.1030, 520.4970] | 509.8164 [502.1030, 520.4970] | 8 | 2592.0 [2510, 2829] | 1415.2 [757, 2078] | 1172.0 [860, 1500] | 42220.8 [41446, 42700] |
| `fresh` | 5 | 512.5632 [506.2700, 529.4150] | 512.5632 [506.2700, 529.4150] | 8 | 2596.0 [2525, 2829] | 1480.2 [757, 2035] | 1172.0 [860, 1500] | 42090.2 [41301, 42844] |
| `fresh_ld` | 5 | 512.5044 [506.1400, 529.3780] | 512.5044 [506.1400, 529.3780] | 8 | 2596.0 [2525, 2829] | 1480.2 [757, 2035] | 1172.0 [860, 1500] | 42127.4 [41342, 42953] |
| `fresh_so` | 5 | 512.5632 [506.2700, 529.4150] | 512.5632 [506.2700, 529.4150] | 8 | 2596.0 [2525, 2829] | 1480.2 [757, 2035] | 1172.0 [860, 1500] | 42090.2 [41301, 42844] |
| `fifo` | 5 | 517.1882 [503.8290, 535.9230] | 517.1882 [503.8290, 535.9230] | 8 | 2595.0 [2510, 2829] | 1790.6 [1032, 2600] | 1236.0 [860, 1750] | 42017.0 [41515, 42544] |

## Metric glossary

* `recall`: rx_ev_delivered / ev_generated (denominator: all generated events, S8.5)
* `on_time_rate`: rx_ev_on_time / ev_generated; on time iff rx_time <= deadline_abs (S7.3)
* `rx_ev_late`: events delivered after their deadline (still delivered, never counted as on time)
* `ev_rejected_full`: events generated while all FL_EVENT_CAPACITY slots were busy (ID burned, S6.3)
* `ev_retention_expired`: events whose retention_abs passed before an ACK (S6.5)
* `ev_retry_exhausted`: events whose last permitted attempt timed out (S6.5)
* `ev_pending_end`: events still pending at the end of the run (censored, reported, S8.7)
* `delivered_but_unacked`: receiver-delivered IDs whose sender outcome is not ACKED (S7.4)
* `lat_mean_ms`: mean first-delivery latency rx_time - gen_time over delivered events
* `event_retries`: event transmissions beyond the first attempt (S8.5)
* `aoi_mean_ms`: time-weighted mean AoI at the receiver averaged over ALL configured streams; NA whenever any stream never received a snapshot (S8.6). Conditional on the interval after each stream's first reception: always read next to unknown_ms_sum
* `aoi_mean_defined_ms`: PARTIAL aggregate over the streams that did receive a snapshot (coverage = aoi_defined_streams); not comparable as full-stream AoI
* `aoi_defined_streams`: number of configured streams with at least one applied snapshot in the run
* `aoi_peak_ms`: largest AoI reached on any stream (S8.6)
* `over_threshold_ms_sum`: continuous-time ms with AoI > aoi_threshold_ms, summed over streams
* `unknown_ms_sum`: ms before the first applied snapshot, summed over streams (warm-up, reported)
* `bytes_total_tx`: bytes handed to the transport in both directions; bytes are not energy (S8.3)

_HOST SIMULATION only. Exploratory/pilot matrix on development seeds; not a held-out evaluation. Bytes are not energy. No performance or superiority claim._
