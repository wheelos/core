# Pod(ICEORYX) vs Protobuf(SHM) Benchmark Report

## Scope

This report captures the deep comparison between:

- **Protobuf over SHM** (`message_type=protobuf`, `transport_mode=shm`)
- **Pod over ICEORYX** (`message_type=pod`, `transport_mode=iceoryx`)

Data source: `/tmp/tests_perf_deep_compare.json` (generated in this session).

## Test method

All cases run through `//tests/perf_test:cyber_rt_benchmark_suite` with real inter-process workers and fixed CPU pinning.

### 1. Flood mode / Unthrottled

- Method: publisher uses `frequency_hz=0` (no sleep/rate limit), continuous send in tight loop.
- Goal: observe **max Msg/s** and **max MB/s** at saturation.
- Cases:
  - max-msgps focus: `payload=1MB`
  - max-mbps focus: `payload=7MB` (common comparable payload)

### 2. Payload sweep

- Method: fixed `100Hz`, payload sweep `1MB, 5MB, 10MB, 20MB, 50MB, 100MB`.
- Observation:
  - CPU vs payload
  - tail latency vs payload (`P99`, `P99.9`)
- Important constraint:
  - ICEORYX chunk payload cap in current implementation is ~7MB, so Pod cases above 7MB are intentionally marked as `skipped_for_iceoryx_payload_cap_mb=7`.

### 3. Fanout scaling (1->N)

- Method: fixed `7MB @ 220Hz`, increase subscribers `N={1,3,5,8}`.
- Observation:
  - throughput scaling
  - loss behavior
  - CPU growth
  - tail latency (`P99`, `P99.9`)

### 4. Tail latency / jitter

- Method: sender writes timestamp before transmit; receiver computes end-to-end latency on first receive callback.
- Metrics: `P99`, `P99.9` (plus min/p50/p95/max).

## Commands used (representative)

```bash
bazel run //tests/perf_test:cyber_rt_benchmark_suite -- \
  --output_json=/tmp/tests_perf_deep_compare.json \
  --run_frequency_sweep=false \
  --run_bandwidth_sweep=false \
  --run_subscriber_scaling=false \
  --run_publisher_scaling=false \
  --run_cpu_interference=false \
  --run_flood_mode_comparison=true \
  --run_payload_sweep_comparison=true \
  --run_fanout_scaling_comparison=true \
  --scaling_case_duration_s=5 \
  --scaling_frequency_hz=220 \
  --scaling_payload_bytes=7340032 \
  --bandwidth_min_mb=1 \
  --bandwidth_max_mb=7 \
  --bandwidth_step_mb=1 \
  --bandwidth_case_duration_s=3 \
  --bandwidth_frequency_hz=220 \
  --payload_sweep_frequency_hz=100 \
  --payload_sweep_duration_s=3 \
  --flood_duration_s=5 \
  --flood_msg_payload_bytes=1048576 \
  --flood_bw_payload_mb=7 \
  --fanout_frequency_hz=220 \
  --fanout_payload_mb=7 \
  --fanout_duration_s=5
```

## Result summary

### Baseline comparison (7MB @ 220Hz, 1->1, 5s)

| Metric | Protobuf(SHM) | Pod(ICEORYX) |
|---|---:|---:|
| Throughput | 1540 MB/s | 1540 MB/s |
| CPU util | 43.9959% | **24.8511%** |
| RSS end | 810372 KB | **315256 KB** |
| Involuntary ctx switches | 2086 | **909** |
| P99 latency | 27.233536 ms | **2.172779 ms** |
| P99.9 latency | 31.101048 ms | **3.527698 ms** |
| Zero-copy evidence | none | `zero_copy_borrowed_messages=1100`, `zero_copy_copy_count=0` |

### Flood mode (unthrottled)

| Case | Protobuf(SHM) | Pod(ICEORYX) |
|---|---|---|
| 1MB max-msgps | 6258.8 msg/s, 6258.8 MB/s, CPU 98.47%, loss 99 | **13076.2 msg/s, 13076.2 MB/s**, CPU 79.98%, loss 2302 |
| 7MB max-mbps | 601.2 msg/s, 4208.4 MB/s, CPU 102.03%, loss 3434 | **4405.6 msg/s, 30839.2 MB/s**, CPU 98.75%, loss 0 |

### Payload sweep (100Hz)

- Protobuf:
  - 1MB -> 100MB shows steep CPU growth and tail-latency deterioration.
  - At 50MB/100MB, system near saturation (`~101% CPU`), P99.9 reaches `~76.8ms` and `~152.6ms`.
- Pod(ICEORYX):
  - 1MB and 5MB remain substantially lower CPU and lower tail latency than protobuf.
  - >7MB cases skipped by design (current ICEORYX payload cap).

### Fanout scaling (7MB @ 220Hz)

| N subscribers | Protobuf(SHM) | Pod(ICEORYX) |
|---:|---|---|
| 1 | 1540 MB/s, loss 0, CPU 44.84%, P99.9 13.13ms | 1540 MB/s, loss 0, CPU 25.37%, P99.9 3.37ms |
| 3 | 4620 MB/s, loss 90, CPU 64.44% | 4620 MB/s, loss 0, CPU 28.37% |
| 5 | 7687.4 MB/s, loss 734, CPU 94.85% | 7700 MB/s, loss 0, CPU 33.38% |
| 8 | 7652.4 MB/s, loss 3751, CPU 106.03%, P99.9 384.85ms | **12290.6 MB/s**, loss 21, CPU 37.28%, P99.9 5.03ms |

## Conclusions

1. **Zero-copy path is active for Pod+ICEORYX** in tested cases (`borrowed>0`, `copy_count=0`).
2. Under equivalent traffic, **Pod+ICEORYX has significantly better CPU and tail latency** than Protobuf+SHM.
3. Under fanout scaling, Pod+ICEORYX keeps much better stability (lower loss, lower tail jitter, slower CPU growth).
4. For large-payload sweep above 7MB, direct Pod-vs-Protobuf comparison is currently bounded by ICEORYX payload-cap implementation; this is an architecture constraint, not a measurement artifact.

## Notes for future runs

- Keep `transport_mode` evidence in result notes (`pub_transport_mode_seen`, `sub_transport_mode_seen`).
- Always include `P99` and `P99.9` in comparison summaries; mean/median alone hide control-loop risk.
- If >7MB Pod traffic is required, increase ICEORYX chunk-cap config and re-run the same suite for apples-to-apples curves.
