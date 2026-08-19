#!/usr/bin/env bash
set -euo pipefail

OUTDIR=""
QUICK=true
LARGE_MESSAGE_COMPARISON=true

while [ "$#" -gt 0 ]; do
  case "$1" in
    --outdir) OUTDIR="$2"; shift 2 ;;
    --outdir=*) OUTDIR="${1#*=}"; shift ;;
    --full) QUICK=false; shift ;;
    --quick) QUICK=true; shift ;;
    --skip-large-message-comparison) LARGE_MESSAGE_COMPARISON=false; shift ;;
    -h|--help)
      echo "Usage: $0 [--outdir DIR] [--full] [--skip-large-message-comparison]"
      exit 0
      ;;
    *)
      echo "Unknown arg: $1" >&2
      exit 1
      ;;
  esac
done

REPO_ROOT=$(git rev-parse --show-toplevel)
cd "$REPO_ROOT"

if [ -z "$OUTDIR" ]; then
  OUTDIR="artifacts/performance/$(date -u +%Y%m%dT%H%M%SZ)"
fi
if [[ "$OUTDIR" != /* ]]; then
  OUTDIR="$REPO_ROOT/$OUTDIR"
fi
mkdir -p "$OUTDIR"

bazel build --config=ci \
  //tests/perf_test:benchmark_monitor \
  //tests/perf_test:benchmark_pub \
  //tests/perf_test:benchmark_sub

BENCHMARK_ARGS=("--output_json=$OUTDIR/baseline.json")
if [ "$QUICK" = true ]; then
  BENCHMARK_ARGS+=("--quick")
fi
if [ "$LARGE_MESSAGE_COMPARISON" = true ]; then
  BENCHMARK_ARGS+=(
    "--run_payload_sweep_comparison=true"
    "--payload_sweep_sizes_mb=1,4,7"
    "--payload_sweep_frequency_hz=30"
    "--payload_sweep_duration_s=3"
  )
fi

CYBER_PATH="$REPO_ROOT/cyber" \
  "$REPO_ROOT/bazel-bin/tests/perf_test/benchmark_monitor" \
  "${BENCHMARK_ARGS[@]}" | tee "$OUTDIR/benchmark.log"

python3 - "$OUTDIR/baseline.json" "$OUTDIR/summary.md" <<'PY'
import json
import sys
from collections import defaultdict

results = json.load(open(sys.argv[1], encoding="utf-8"))["results"]
groups = defaultdict(lambda: [0, 0, 0, 0.0, 0.0, 0.0])
for result in results:
    group = groups[(result["coverage"], result["message_type"])]
    group[0] += 1
    group[1] += int(result["success"])
    group[2] += result["reliability"]["total_loss"]
    group[3] = max(group[3], result["latency"]["p99_ns"])
    group[4] = max(group[4], result["throughput"]["messages_per_s"])
    group[5] = max(group[5], result["reliability"]["loss_rate"])

with open(sys.argv[2], "w", encoding="utf-8") as summary:
    summary.write("# Cyber RT Performance Baseline\n\n")
    summary.write(f"Cases: {len(results)}; passed: "
                  f"{sum(item['success'] for item in results)}\n\n")
    summary.write("| Coverage | Message | Cases | Passed | Max p99 (ns) | "
                  "Max msg/s | Total loss | Max loss rate |\n")
    summary.write("| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |\n")
    for (coverage, message), values in sorted(groups.items()):
        summary.write(
            f"| {coverage} | {message} | {values[0]} | {values[1]} | "
            f"{values[3]:.0f} | {values[4]:.1f} | {values[2]} | "
            f"{values[5]:.6f} |\n"
        )

payloads = defaultdict(dict)
for result in results:
    if result["scenario"] == "payload_sweep":
        payloads[result["payload_bytes"] // (1024 * 1024)][
            result["message_type"]
        ] = result

if payloads:
    with open(sys.argv[2], "a", encoding="utf-8") as summary:
        summary.write(
            "\n## Large-message comparison: SHM Protobuf vs Iceoryx POD\n\n"
        )
        summary.write("| Payload (MiB) | Protobuf p99 (ms) | POD p99 (ms) | "
                      "Protobuf MiB/s | POD MiB/s | Protobuf loss | POD loss | "
                      "POD zero-copy |\n")
        summary.write(
            "| ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |\n"
        )
        for payload, pair in sorted(payloads.items()):
            protobuf = pair.get("protobuf")
            pod = pair.get("pod")

            def value(result, group, key, scale=1.0):
                if result is None:
                    return "n/a"
                return f"{result[group][key] / scale:.3f}"

            pod_zero_copy = (
                "yes" if pod and "zero_copy_copy_count=0" in pod["notes"]
                else "no" if pod else "n/a"
            )
            summary.write(
                f"| {payload} | "
                f"{value(protobuf, 'latency', 'p99_ns', 1e6)} | "
                f"{value(pod, 'latency', 'p99_ns', 1e6)} | "
                f"{value(protobuf, 'throughput', 'mb_per_s')} | "
                f"{value(pod, 'throughput', 'mb_per_s')} | "
                f"{value(protobuf, 'reliability', 'loss_rate')} | "
                f"{value(pod, 'reliability', 'loss_rate')} | {pod_zero_copy} |\n"
            )
PY

python3 - "$OUTDIR/baseline.json" <<'PY'
import json
import sys

failed = [
    result
    for result in json.load(open(sys.argv[1], encoding="utf-8"))["results"]
    if not result["success"]
]
if failed:
    for result in failed:
        print(
            "Performance case failed:",
            result["scenario"],
            result["coverage"],
            result["message_type"],
            result["payload_bytes"],
            result["error_message"],
            file=sys.stderr,
        )
    sys.exit(1)
PY

{
  echo "generated_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "git_sha=$(git rev-parse HEAD)"
  echo "bazel_version=$(bazel --version)"
  echo "mode=$([ "$QUICK" = true ] && echo quick || echo full)"
  uname -a
} > "$OUTDIR/metadata.txt"

echo "Performance baseline reports: $OUTDIR/baseline.json and $OUTDIR/summary.md"
