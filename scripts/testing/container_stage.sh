#!/usr/bin/env bash
set -euo pipefail

export PATH="/workspace/scripts/testing:$PATH"
export GLOG_log_dir=/results/logs
mkdir -p "$GLOG_log_dir"
expected="$(cat /workspace/.bazelversion)"
actual="$(/usr/local/bin/bazel --version)"
if [ "$actual" != "bazel $expected" ]; then
  echo "Expected Bazel $expected, found $actual; rebuild the test image." >&2
  exit 1
fi
bash scripts/release/check_bzlmod_lockfile.sh --check
collect_logs() {
  if [ -f /results/bazel-events.json ]; then
    python3 - <<'PY'
import json
from pathlib import Path
import shutil

for line in Path("/results/bazel-events.json").read_text().splitlines():
    event = json.loads(line)
    if "testSummary" not in event:
        continue
    label = event["id"]["testSummary"]["label"]
    if label.startswith("//"):
        relative = label[2:].replace(":", "/")
        source = Path("bazel-testlogs") / relative
        if source.is_dir():
            shutil.copytree(source, Path("/results/testlogs") / relative,
                            dirs_exist_ok=True)
PY
  fi
}
trap collect_logs EXIT

case "$1" in
  unit)
    python3 scripts/testing/check_test_layers.py
    bazel test --config=ci //tests:unit --build_event_json_file=/results/bazel-events.json
    ;;
  integration)
    bazel build --config=ci //cyber //:wheelos_core
    bazel test --config=ci //tests:integration //tests:performance_regression \
      --nocache_test_results \
      --build_event_json_file=/results/bazel-events.json
    ;;
  diagnostic)
    bazel test --config=ci "${DIAGNOSTIC_TARGET:?}" --nocache_test_results \
      --build_event_json_file=/results/bazel-events.json
    ;;
  package)
    publish_args=()
    if [ "${PUBLISH_RELEASE:-false}" = true ]; then
      publish_args+=(--publish)
    fi
    bash scripts/release/build_release_artifacts.sh --skip-baseline \
      --outdir /candidate/release --distdir /cache/distdir "${publish_args[@]}"
    bazel build --config=ci //tests/e2e:fixtures
    mkdir -p /fixtures
    fixture_files="$(bazel cquery --config=ci //tests/e2e:fixtures --output=files)"
    test -n "$fixture_files"
    while IFS= read -r artifact; do
      cp --remove-destination -L "$artifact" /fixtures/
    done <<< "$fixture_files"
    ;;
  wheel)
    wheel_args=(--skip-sdist --outdir /candidate/release/pycyber)
    if [ "${PUBLISH_RELEASE:-false}" = true ]; then
      wheel_args+=(--publish)
    fi
    bash scripts/release/build_and_package_pycyber.sh "${wheel_args[@]}"
    ;;
  perf)
    bash scripts/release/run_release_acceptance.sh --stage perf --outdir /results/acceptance
    source scripts/env/runtime.bash
    python3 - /results/long-run-timing.json \
      bazel-bin/tests/perf_test/benchmark_monitor \
      --output_json=/results/long-run.json \
      --enable_long_run=true --long_run_seconds="${LONG_RUN_SECONDS:-7200}" \
      --long_run_frequency_hz=200 --long_run_payload_bytes=1024 \
      --run_comparison_bandwidth_search=false \
      --run_frequency_sweep=false --run_bandwidth_sweep=false \
      --run_subscriber_scaling=false --run_publisher_scaling=false \
      --run_cpu_interference=false <<'PY'
import json
import subprocess
import sys
import time
from pathlib import Path

started = time.monotonic()
completed = subprocess.run(sys.argv[2:], check=False)
Path(sys.argv[1]).write_text(json.dumps({
    "elapsed_seconds": time.monotonic() - started,
}))
raise SystemExit(completed.returncode)
PY
    ;;
  *)
    echo "Unknown container stage: $1" >&2
    exit 2
    ;;
esac
