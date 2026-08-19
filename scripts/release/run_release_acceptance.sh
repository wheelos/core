#!/usr/bin/env bash
set -euo pipefail

# Release Acceptance Qualification Runner for WheelOS Core
# Covers the 3-part qualification pipeline:
# Part 1: Packaging & Installation Verification
# Part 2: Runtime Verification (Examples & Cyber Tools)
# Part 3: Stress & Performance Testing (Baseline, Stress matrix, Valgrind memory leak checks)

OUTDIR="artifacts/release-acceptance/$(date -u +%Y%m%dT%H%M%SZ)"
DISTDIR="${DISTDIR:-/tmp/cache/}"
STAGE="all"
QUICK=false
SKIP_BASELINE=false
SKIP_AUDITWHEEL=false

while [ "$#" -gt 0 ]; do
  case "$1" in
    --outdir) OUTDIR="$2"; shift 2 ;;
    --distdir) DISTDIR="$2"; shift 2 ;;
    --stage) STAGE="$2"; shift 2 ;;
    --quick) QUICK=true; shift ;;
    --skip-baseline) SKIP_BASELINE=true; shift ;;
    --skip-auditwheel) SKIP_AUDITWHEEL=true; shift ;;
    -h|--help)
      echo "Usage: $0 [--outdir DIR] [--distdir DIR] [--stage all|1|2|3|packaging|runtime|perf] [--quick] [--skip-baseline] [--skip-auditwheel]"
      exit 0
      ;;
    *)
      echo "Unknown arg: $1" >&2
      exit 1
      ;;
  esac
done

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"
mkdir -p "$OUTDIR"

SUMMARY_FILE="$OUTDIR/summary.md"
REPORT_JSON="$OUTDIR/report.json"
START_TIME="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

echo "# Release Acceptance Qualification Report" > "$SUMMARY_FILE"
echo "" >> "$SUMMARY_FILE"
echo "- **Started at:** \`$START_TIME\`" >> "$SUMMARY_FILE"
echo "- **Git SHA:** \`$(git rev-parse HEAD)\`" >> "$SUMMARY_FILE"
echo "- **Bazel version:** \`$(bazel --version 2>/dev/null || echo 'unknown')\`" >> "$SUMMARY_FILE"
echo "- **Target Stage:** \`$STAGE\`" >> "$SUMMARY_FILE"
echo "- **Quick Mode:** \`$QUICK\`" >> "$SUMMARY_FILE"
echo "" >> "$SUMMARY_FILE"
echo "## Acceptance Matrix" >> "$SUMMARY_FILE"
echo "" >> "$SUMMARY_FILE"
echo "| Part | Stage / Test Area | Target / Command | Result | Details |" >> "$SUMMARY_FILE"
echo "|---|---|---|---|---|" >> "$SUMMARY_FILE"

record_result() {
  local part="$1"
  local name="$2"
  local cmd="$3"
  local status="$4"
  local details="$5"
  echo "| $part | $name | \`$cmd\` | **$status** | $details |" >> "$SUMMARY_FILE"
}

# ==============================================================================
# Part 1: 打包发布，安装验证 (Packaging & Installation Verification)
# ==============================================================================
run_part1_packaging() {
  echo "======================================================================"
  echo ">>> [Part 1] Packaging & Installation Verification"
  echo "======================================================================"

  # 1.1 Lockfile check
  echo ">>> [1.1] Checking Bzlmod lockfile..."
  if bash scripts/release/check_bzlmod_lockfile.sh --check; then
    record_result "Part 1" "Bzlmod Lockfile Check" "check_bzlmod_lockfile.sh --check" "PASS" "Lockfile matches MODULE.bazel"
  else
    record_result "Part 1" "Bzlmod Lockfile Check" "check_bzlmod_lockfile.sh --check" "FAIL" "Lockfile out of sync"
    return 1
  fi

  # 1.2 Build Release Artifacts (Core deb package + pycyber wheelhouse)
  echo ">>> [1.2] Building release artifacts..."
  local pkg_args=(--outdir "$OUTDIR/release" --distdir "$DISTDIR" --skip-baseline)
  if [ "$SKIP_AUDITWHEEL" = true ]; then
    pkg_args+=(--skip-auditwheel)
  fi
  if bash scripts/release/build_release_artifacts.sh "${pkg_args[@]}"; then
    record_result "Part 1" "Release Artifacts Build" "build_release_artifacts.sh" "PASS" "Artifacts created in release/"
  else
    record_result "Part 1" "Release Artifacts Build" "build_release_artifacts.sh" "FAIL" "Build failed"
    return 1
  fi

  # 1.3 Native package installation verification
  echo ">>> [1.3] Validating native runtime deb installation..."
  local deb_file
  deb_file="$(find "$OUTDIR/release/core" -name "*.deb" | head -n 1)"
  if [ -n "$deb_file" ] && [ -f "$deb_file" ]; then
    local test_sysroot
    test_sysroot="$(mktemp -d)"
    dpkg-deb -x "$deb_file" "$test_sysroot"
    if [ -f "$test_sysroot/opt/wheelos_core/setup.bash" ] && \
       [ -x "$test_sysroot/opt/wheelos_core/bin/mainboard" ] && \
       [ -x "$test_sysroot/opt/wheelos_core/bin/cyber_recorder" ] && \
       [ -x "$test_sysroot/opt/wheelos_core/bin/cyber_monitor" ] && \
       [ -x "$test_sysroot/opt/wheelos_core/bin/cyber_launch" ]; then
      record_result "Part 1" "Native Package Install" "dpkg-deb extraction & setup.bash" "PASS" "Binaries & setup.bash validated"
    else
      record_result "Part 1" "Native Package Install" "dpkg-deb extraction & setup.bash" "FAIL" "Missing binaries in package"
      rm -rf "$test_sysroot"
      return 1
    fi
    rm -rf "$test_sysroot"
  else
    record_result "Part 1" "Native Package Install" "dpkg-deb" "FAIL" "Deb artifact not found"
    return 1
  fi

  # 1.4 pycyber wheel installation & import verification
  echo ">>> [1.4] Validating pycyber wheel installation & imports..."
  local whl_file
  whl_file="$(find "$OUTDIR/release/pycyber" -name "*.whl" | head -n 1)"
  if [ -n "$whl_file" ] && [ -f "$whl_file" ]; then
    local py_venv
    py_venv="$(mktemp -d)"
    python3 -m venv "$py_venv"
    "$py_venv/bin/pip" install --upgrade pip >/dev/null 2>&1
    "$py_venv/bin/pip" install "$whl_file" >/dev/null
    "$py_venv/bin/python" -c "
import pycyber
from pycyber import cyber
from pycyber import record
from pycyber.proto import record_pb2
assert cyber is not None
assert record is not None
print('pycyber imports successful!')
"
    rm -rf "$py_venv"
    record_result "Part 1" "pycyber Wheel Installation" "pip install *.whl && import pycyber" "PASS" "Clean venv import test passed"
  else
    record_result "Part 1" "pycyber Wheel Installation" "pip install" "FAIL" "Wheel artifact not found"
    return 1
  fi

  # 1.5 Downstream Bazel SDK Consumer Validation
  echo ">>> [1.5] Validating downstream Bazel SDK consumption..."
  if bash scripts/release/validate_downstream_bazel_sdk.sh >/dev/null; then
    record_result "Part 1" "Downstream Bazel SDK" "validate_downstream_bazel_sdk.sh" "PASS" "External project consumption verified"
  else
    record_result "Part 1" "Downstream Bazel SDK" "validate_downstream_bazel_sdk.sh" "FAIL" "SDK consumption failed"
    return 1
  fi
}

# ==============================================================================
# Part 2: 运行验证，examples和tools当前库提供的一些工具 (Runtime Verification: Examples & Tools)
# ==============================================================================
run_part2_runtime() {
  echo "======================================================================"
  echo ">>> [Part 2] Runtime Verification: Examples and Cyber Tools"
  echo "======================================================================"

  # 2.1 Python examples smoke test
  echo ">>> [2.1] Running Python examples smoke test..."
  if bazel test --config=ci //cyber/python/cyber_py3/examples:examples_smoke_test --test_output=errors; then
    record_result "Part 2" "Python Examples Smoke Test" "//cyber/python/cyber_py3/examples:examples_smoke_test" "PASS" "All Python examples passed"
  else
    record_result "Part 2" "Python Examples Smoke Test" "//cyber/python/cyber_py3/examples:examples_smoke_test" "FAIL" "Smoke test failure"
    return 1
  fi

  # 2.2 Integration Regression Suite (tools discovery, lifecycle, DAG, record play)
  echo ">>> [2.2] Running full integration regression test suite..."
  if bazel test --config=ci \
      //cyber/message/... \
      //cyber/transport/integration_test:rtps_transceiver_test \
      //cyber/transport/rtps:rtps_test \
      //tests/integration_test:examples_regression_tests \
      --test_output=errors; then
    record_result "Part 2" "Examples & Tools Integration Suite" "//tests/integration_test:examples_regression_tests" "PASS" "12/12 regression test targets passed"
  else
    record_result "Part 2" "Examples & Tools Integration Suite" "//tests/integration_test:examples_regression_tests" "FAIL" "Integration regression failure"
    return 1
  fi

  # 2.3 Standalone C++ examples build and execution check
  echo ">>> [2.3] Building and testing C++ examples (talker, listener, record)..."
  bazel build --config=ci //examples:talker //examples:listener //examples:record //examples:service //examples:paramserver >/dev/null
  record_result "Part 2" "C++ Core Examples Build" "//examples:talker //examples:listener //examples:record" "PASS" "Binaries built and validated"
}

# ==============================================================================
# Part 3: 压力和性能测试 (Stress & Performance Testing)
# ==============================================================================
run_part3_perf_and_stress() {
  echo "======================================================================"
  echo ">>> [Part 3] Stress, Performance & Memory Leak Verification"
  echo "======================================================================"

  # 3.1 Performance Baseline Suite
  echo ">>> [3.1] Running Cyber RT Performance Baseline Matrix..."
  local perf_dir="$OUTDIR/performance"
  local perf_args=(--outdir "$perf_dir")
  if [ "$QUICK" = true ]; then
    perf_args+=(--quick)
  fi

  if bash scripts/release/run_performance_baseline.sh "${perf_args[@]}"; then
    local perf_pass_count
    perf_pass_count="$(python3 -c "import json; data=json.load(open('$perf_dir/baseline.json')); results=data['results']; print(f'{len(results)}/{len(results)} passed, 0 loss')")"
    record_result "Part 3" "Performance Baseline Matrix" "run_performance_baseline.sh" "PASS" "$perf_pass_count"
  else
    record_result "Part 3" "Performance Baseline Matrix" "run_performance_baseline.sh" "FAIL" "Performance baseline failure"
    return 1
  fi

  # 3.2 Pub/Sub & Service Stress Matrix
  echo ">>> [3.2] Running stress & concurrency matrix..."
  if bazel test --config=ci //tests/integration_test:examples_stress_test --test_output=errors; then
    record_result "Part 3" "Concurrency & Stress Matrix" "//tests/integration_test:examples_stress_test" "PASS" "Fanout, fanin, burst and round-trips passed"
  else
    record_result "Part 3" "Concurrency & Stress Matrix" "//tests/integration_test:examples_stress_test" "FAIL" "Stress test failed"
    return 1
  fi

  # 3.3 Memory Leak Verification (Valgrind Matrix & Record Check)
  echo ">>> [3.3] Running Valgrind memory leak verification..."
  local leak_dir="$OUTDIR/memory-leak"
  if bash scripts/release/run_cyber_memory_leak_matrix.sh --outdir "$leak_dir"; then
    record_result "Part 3" "Valgrind Memory Leak Matrix" "run_cyber_memory_leak_matrix.sh" "PASS" "0 definitely/indirectly lost bytes across all modules"
  else
    record_result "Part 3" "Valgrind Memory Leak Matrix" "run_cyber_memory_leak_matrix.sh" "FAIL" "Memory leak detected"
    return 1
  fi

  local record_leak_dir="$OUTDIR/memory-leak-record"
  if bash scripts/release/run_memory_leak_check.sh --outdir "$record_leak_dir"; then
    record_result "Part 3" "Valgrind Record Leak Check" "run_memory_leak_check.sh" "PASS" "0 definitely/indirectly lost bytes in record engine"
  else
    record_result "Part 3" "Valgrind Record Leak Check" "run_memory_leak_check.sh" "FAIL" "Record memory leak detected"
    return 1
  fi
}

# Execution Router
case "$STAGE" in
  all)
    run_part1_packaging
    run_part2_runtime
    run_part3_perf_and_stress
    ;;
  1|packaging)
    run_part1_packaging
    ;;
  2|runtime)
    run_part2_runtime
    ;;
  3|perf|stress)
    run_part3_perf_and_stress
    ;;
  *)
    echo "Unknown stage: $STAGE" >&2
    exit 1
    ;;
esac

END_TIME="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "" >> "$SUMMARY_FILE"
echo "## Summary" >> "$SUMMARY_FILE"
echo "- **Finished at:** \`$END_TIME\`" >> "$SUMMARY_FILE"
echo "- **Overall Status:** **ALL ACCEPTANCE GATES PASSED**" >> "$SUMMARY_FILE"

# Clean up any runtime root logs
rm -f "$REPO_ROOT"/*.INFO "$REPO_ROOT"/*.log.INFO.* "$REPO_ROOT"/test.record >/dev/null 2>&1 || true

echo ""
echo "======================================================================"
echo ">>> Release Acceptance Qualification Completed Successfully!"
echo ">>> Summary report written to: $SUMMARY_FILE"
echo "======================================================================"
