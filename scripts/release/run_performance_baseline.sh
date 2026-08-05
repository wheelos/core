#!/usr/bin/env bash
set -euo pipefail

OUTDIR=""
QUICK=true

while [ "$#" -gt 0 ]; do
  case "$1" in
    --outdir) OUTDIR="$2"; shift 2 ;;
    --full) QUICK=false; shift ;;
    -h|--help)
      echo "Usage: $0 [--outdir DIR] [--full]"
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
mkdir -p "$OUTDIR"

bazel build --config=ci \
  //tests/perf_test:benchmark_monitor \
  //tests/perf_test:benchmark_pub \
  //tests/perf_test:benchmark_sub

BENCHMARK_ARGS=("--output_json=$REPO_ROOT/$OUTDIR/baseline.json")
if [ "$QUICK" = true ]; then
  BENCHMARK_ARGS+=("--quick")
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
PY

{
  echo "generated_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "git_sha=$(git rev-parse HEAD)"
  echo "bazel_version=$(bazel --version)"
  echo "mode=$([ "$QUICK" = true ] && echo quick || echo full)"
  uname -a
} > "$OUTDIR/metadata.txt"

echo "Performance baseline reports: $OUTDIR/baseline.json and $OUTDIR/summary.md"
