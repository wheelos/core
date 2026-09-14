#!/usr/bin/env bash
set -euo pipefail

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

case "$STAGE" in
  all|1|2|3|packaging|runtime|perf|stress) ;;
  *)
    echo "Unknown stage: $STAGE" >&2
    exit 1
    ;;
esac

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"
mkdir -p "$OUTDIR"
OUTDIR="$(cd "$OUTDIR" && pwd)"

SUMMARY_FILE="$OUTDIR/summary.md"
REPORT_JSON="$OUTDIR/report.json"
RESULTS_FILE="$OUTDIR/.stage-results.jsonl"
START_TIME="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
GIT_SHA="$(git rev-parse HEAD)"
BAZEL_VERSION="$(bazel --version 2>/dev/null || echo unknown)"
RUNTIME_MATRIX_COMPLETED=false
AUDITWHEEL_SKIPPED=false
FIXTURE_PREPARED=false
FIXTURE_RECORDED=false
FIXTURE_MODE="not_requested"
FIXTURE_PATH=""
FIXTURE_SELF_CONTAINED_STATUS="not_run"
FIXTURE_EXTERNAL_STATUS="not_run"
FIXTURE_BAZEL_ARGS=()

: > "$RESULTS_FILE"
cat > "$SUMMARY_FILE" <<EOF
# Release Acceptance Qualification Report

- **Started at:** \`$START_TIME\`
- **Git SHA:** \`$GIT_SHA\`
- **Bazel version:** \`$BAZEL_VERSION\`
- **Target Stage:** \`$STAGE\`
- **Quick Mode:** \`$QUICK\`
- **Skip Baseline:** \`$SKIP_BASELINE\`
- **Skip Auditwheel:** \`$SKIP_AUDITWHEEL\`

## Acceptance Matrix

| Part | Stage / Test Area | Target / Command | Result | Details |
|---|---|---|---|---|
EOF

record_result() {
  local part="$1"
  local name="$2"
  local cmd="$3"
  local status="$4"
  local details="$5"
  local markdown_details="${details//|/\\|}"
  echo "| $part | $name | \`$cmd\` | **$status** | $markdown_details |" >> "$SUMMARY_FILE"
  python3 - "$RESULTS_FILE" "$part" "$name" "$cmd" "$status" "$details" <<'PY'
import json
import sys

path, part, name, command, status, details = sys.argv[1:]
with open(path, "a", encoding="utf-8") as output:
    output.write(
        json.dumps(
            {
                "part": part,
                "name": name,
                "command": command,
                "status": status.lower(),
                "details": details,
            },
            sort_keys=True,
        )
        + "\n"
    )
PY
}

finalize_report() {
  local exit_code="$1"
  local end_time
  local overall_status="passed"
  trap - EXIT
  set +e
  end_time="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  if [ "$exit_code" -ne 0 ]; then
    overall_status="failed"
  elif [ "$AUDITWHEEL_SKIPPED" = true ]; then
    overall_status="partial"
  fi

  python3 - \
    "$RESULTS_FILE" "$REPORT_JSON" "$START_TIME" "$end_time" \
    "$GIT_SHA" "$BAZEL_VERSION" "$STAGE" "$QUICK" "$SKIP_BASELINE" \
    "$SKIP_AUDITWHEEL" \
    "$overall_status" "$exit_code" "$FIXTURE_MODE" "$FIXTURE_PATH" \
    "$FIXTURE_SELF_CONTAINED_STATUS" "$FIXTURE_EXTERNAL_STATUS" <<'PY'
import json
import sys

(
    results_path,
    report_path,
    started_at,
    finished_at,
    git_sha,
    bazel_version,
    target_stage,
    quick,
    skip_baseline,
    skip_auditwheel,
    overall_status,
    exit_code,
    fixture_mode,
    fixture_path,
    self_contained_status,
    external_status,
) = sys.argv[1:]

stage_results = []
with open(results_path, encoding="utf-8") as source:
    for line in source:
        if line.strip():
            stage_results.append(json.loads(line))

stages = []
by_part = {}
for result in stage_results:
    part = result["part"]
    if part not in by_part:
        by_part[part] = []
        stages.append({"part": part, "status": "passed"})
    by_part[part].append(result)
for stage in stages:
    statuses = {item["status"] for item in by_part[stage["part"]]}
    if "fail" in statuses:
        stage["status"] = "failed"
    elif statuses == {"skip"}:
        stage["status"] = "skipped"
    elif "skip" in statuses:
        stage["status"] = "partial"

report = {
    "schema_version": 1,
    "started_at": started_at,
    "finished_at": finished_at,
    "git_sha": git_sha,
    "bazel_version": bazel_version,
    "target_stage": target_stage,
    "quick": quick == "true",
    "skip_baseline": skip_baseline == "true",
    "skip_auditwheel": skip_auditwheel == "true",
    "overall_status": overall_status,
    "exit_code": int(exit_code),
    "stages": stages,
    "stage_results": stage_results,
    "fixture_coverage": {
        "mode": fixture_mode,
        "path": fixture_path or None,
        "self_contained": self_contained_status,
        "external": external_status,
    },
}
with open(report_path, "w", encoding="utf-8") as output:
    json.dump(report, output, indent=2, sort_keys=True)
    output.write("\n")
PY

  {
    echo ""
    echo "## Summary"
    echo "- **Finished at:** \`$end_time\`"
    if [ "$overall_status" = "passed" ]; then
      echo "- **Overall Status:** **ALL REQUESTED ACCEPTANCE GATES PASSED**"
    elif [ "$overall_status" = "partial" ]; then
      echo "- **Overall Status:** **PARTIAL — AUDITWHEEL REPAIR SKIPPED**"
    else
      echo "- **Overall Status:** **ACCEPTANCE FAILED OR PARTIAL**"
      echo "- **Exit Code:** \`$exit_code\`"
    fi
  } >> "$SUMMARY_FILE"

  rm -f "$RESULTS_FILE"
  rm -f "$REPO_ROOT"/*.INFO "$REPO_ROOT"/*.log.INFO.* "$REPO_ROOT"/test.record >/dev/null 2>&1 || true
  echo "Summary report: $SUMMARY_FILE"
  echo "JSON report: $REPORT_JSON"
  exit "$exit_code"
}
trap 'finalize_report "$?"' EXIT

prepare_fixture_coverage() {
  if [ "$FIXTURE_PREPARED" = true ]; then
    [ "$FIXTURE_EXTERNAL_STATUS" != "failed" ]
    return
  fi
  FIXTURE_PREPARED=true

  if [[ -v CYBER_RECORD_PLAY_FIXTURE ]]; then
    FIXTURE_MODE="external"
    FIXTURE_PATH="$CYBER_RECORD_PLAY_FIXTURE"
    FIXTURE_SELF_CONTAINED_STATUS="skipped"
    if [ -z "$FIXTURE_PATH" ] || [ ! -f "$FIXTURE_PATH" ]; then
      FIXTURE_EXTERNAL_STATUS="failed"
      record_result "Part 2" "Record Fixture Coverage" \
        "CYBER_RECORD_PLAY_FIXTURE" "FAIL" \
        "Explicit fixture is missing: ${FIXTURE_PATH:-<empty>}"
      FIXTURE_RECORDED=true
      return 1
    fi
    FIXTURE_PATH="$(cd "$(dirname "$FIXTURE_PATH")" && pwd)/$(basename "$FIXTURE_PATH")"
    export CYBER_RECORD_PLAY_FIXTURE="$FIXTURE_PATH"
    FIXTURE_BAZEL_ARGS+=("--test_env=CYBER_RECORD_PLAY_FIXTURE=$FIXTURE_PATH")
    FIXTURE_EXTERNAL_STATUS="pending"
  else
    FIXTURE_MODE="self_contained"
    FIXTURE_SELF_CONTAINED_STATUS="pending"
    FIXTURE_EXTERNAL_STATUS="skipped"
  fi
}

finish_fixture_coverage() {
  local outcome="$1"
  if [ "$FIXTURE_RECORDED" = true ]; then
    return
  fi
  if [ "$FIXTURE_SELF_CONTAINED_STATUS" = "pending" ]; then
    FIXTURE_SELF_CONTAINED_STATUS="$outcome"
  fi
  if [ "$FIXTURE_EXTERNAL_STATUS" = "pending" ]; then
    FIXTURE_EXTERNAL_STATUS="$outcome"
  fi

  if [ "$outcome" = "actual" ]; then
    if [ "$FIXTURE_MODE" = "external" ]; then
      record_result "Part 2" "Record Fixture Coverage" \
        "//examples/record_play:record_play_test" "PASS" \
        "external=actual ($FIXTURE_PATH); self-contained=skipped"
    else
      record_result "Part 2" "Record Fixture Coverage" \
        "//examples/record_play:record_play_test" "PASS" \
        "self-contained=actual; external=skipped (not requested)"
    fi
  else
    record_result "Part 2" "Record Fixture Coverage" \
      "//examples/record_play:record_play_test" "FAIL" \
      "Fixture-backed runtime coverage did not complete"
  fi
  FIXTURE_RECORDED=true
}

run_part1_packaging() {
  echo ">>> [Part 1] Packaging & Installation Verification"

  if bash scripts/release/check_bzlmod_lockfile.sh --check; then
    record_result "Part 1" "Bzlmod Lockfile Check" \
      "check_bzlmod_lockfile.sh --check" "PASS" "Lockfile matches MODULE.bazel"
  else
    record_result "Part 1" "Bzlmod Lockfile Check" \
      "check_bzlmod_lockfile.sh --check" "FAIL" "Lockfile out of sync"
    return 1
  fi

  if [ "$SKIP_BASELINE" = false ]; then
    prepare_fixture_coverage || return 1
  fi

  local pkg_args=(--outdir "$OUTDIR/release" --distdir "$DISTDIR")
  if [ "$SKIP_BASELINE" = true ]; then
    pkg_args+=(--skip-baseline)
  fi
  if [ "$SKIP_AUDITWHEEL" = true ]; then
    pkg_args+=(--skip-auditwheel)
    AUDITWHEEL_SKIPPED=true
    record_result "Part 1" "Auditwheel Repair" \
      "build_release_artifacts.sh --skip-auditwheel" "SKIP" \
      "Release is partial: manylinux wheel repair was explicitly skipped"
  fi
  if bash scripts/release/build_release_artifacts.sh "${pkg_args[@]}"; then
    record_result "Part 1" "Release Artifacts Build" \
      "build_release_artifacts.sh" "PASS" "Artifacts created in release/"
    if [ "$SKIP_BASELINE" = false ]; then
      RUNTIME_MATRIX_COMPLETED=true
      finish_fixture_coverage actual
    fi
  else
    record_result "Part 1" "Release Artifacts Build" \
      "build_release_artifacts.sh" "FAIL" "Build failed"
    if [ "$SKIP_BASELINE" = false ]; then
      finish_fixture_coverage failed
    fi
    return 1
  fi

  local deb_file
  deb_file="$(find "$OUTDIR/release/core" -name "*.deb" -print -quit)"
  if [ -z "$deb_file" ] || [ ! -f "$deb_file" ]; then
    record_result "Part 1" "Native Package Install" "dpkg-deb" "FAIL" "Deb artifact not found"
    return 1
  fi
  local test_sysroot="$OUTDIR/work/native-package"
  rm -rf "$test_sysroot"
  mkdir -p "$test_sysroot"
  dpkg-deb -x "$deb_file" "$test_sysroot"
  if [ -f "$test_sysroot/opt/wheelos_core/setup.bash" ] &&
     [ -x "$test_sysroot/opt/wheelos_core/bin/mainboard" ] &&
     [ -x "$test_sysroot/opt/wheelos_core/bin/cyber_recorder" ] &&
     [ -x "$test_sysroot/opt/wheelos_core/bin/cyber_monitor" ] &&
     [ -x "$test_sysroot/opt/wheelos_core/bin/cyber_launch" ]; then
    record_result "Part 1" "Native Package Install" \
      "dpkg-deb extraction & setup.bash" "PASS" "Binaries and setup.bash validated"
  else
    record_result "Part 1" "Native Package Install" \
      "dpkg-deb extraction & setup.bash" "FAIL" "Missing binaries in package"
    return 1
  fi
  rm -rf "$test_sysroot"

  local whl_file
  whl_file="$(find "$OUTDIR/release/pycyber" -name "*.whl" -print -quit)"
  if [ -z "$whl_file" ] || [ ! -f "$whl_file" ]; then
    record_result "Part 1" "pycyber Wheel Installation" "pip install" "FAIL" "Wheel artifact not found"
    return 1
  fi
  local py_venv="$OUTDIR/work/pycyber-venv"
  rm -rf "$py_venv"
  python3 -m venv "$py_venv"
  "$py_venv/bin/pip" install --upgrade pip >/dev/null 2>&1
  "$py_venv/bin/pip" install "$whl_file" >/dev/null
  "$py_venv/bin/python" -c \
    "import pycyber; from pycyber import cyber, record; from pycyber.proto import record_pb2"
  rm -rf "$py_venv"
  record_result "Part 1" "pycyber Wheel Installation" \
    "pip install *.whl && import pycyber" "PASS" "Clean venv import test passed"

  if bash scripts/release/validate_downstream_bazel_sdk.sh >/dev/null; then
    record_result "Part 1" "Downstream Bazel SDK" \
      "validate_downstream_bazel_sdk.sh" "PASS" "External project consumption verified"
  else
    record_result "Part 1" "Downstream Bazel SDK" \
      "validate_downstream_bazel_sdk.sh" "FAIL" "SDK consumption failed"
    return 1
  fi
}

run_part2_runtime() {
  echo ">>> [Part 2] Runtime Verification: Core Tool Matrix"

  if [ "$RUNTIME_MATRIX_COMPLETED" = true ]; then
    record_result "Part 2" "Core Runtime and Tool Matrix" \
      "//tests/integration_test:core_tool_matrix_tests" "PASS" \
      "Executed once by the successful Ubuntu baseline"
    return
  fi

  prepare_fixture_coverage || return 1
  if bazel test --config=ci --distdir="$DISTDIR" \
      "${FIXTURE_BAZEL_ARGS[@]}" \
      //tests/integration_test:core_tool_matrix_tests \
      --test_output=errors; then
    RUNTIME_MATRIX_COMPLETED=true
    record_result "Part 2" "Core Runtime and Tool Matrix" \
      "//tests/integration_test:core_tool_matrix_tests" "PASS" \
      "Runtime, transport, Python, record/play, examples, and tools passed"
    finish_fixture_coverage actual
  else
    record_result "Part 2" "Core Runtime and Tool Matrix" \
      "//tests/integration_test:core_tool_matrix_tests" "FAIL" \
      "Canonical runtime acceptance target failed"
    finish_fixture_coverage failed
    return 1
  fi
}

run_part3_perf_and_stress() {
  echo ">>> [Part 3] Performance and Memory Leak Verification"

  local perf_dir="$OUTDIR/performance"
  local perf_args=(--outdir "$perf_dir")
  if [ "$QUICK" = true ]; then
    perf_args+=(--quick)
  else
    perf_args+=(--full)
  fi
  if bash scripts/release/run_performance_baseline.sh "${perf_args[@]}"; then
    local perf_pass_count
    perf_pass_count="$(python3 -c "import json; data=json.load(open('$perf_dir/baseline.json')); results=data['results']; required=[item for item in results if item.get('required_for_release', True)]; exploratory=[item for item in results if not item.get('required_for_release', True)]; passed=sum(bool(item['success']) for item in required); stable=sum(bool(item['success']) for item in exploratory); loss=sum(item['reliability']['total_loss'] for item in required); print(f'release gates {passed}/{len(required)} passed, required loss={loss}; exploratory stable {stable}/{len(exploratory)}')")"
    record_result "Part 3" "Performance Baseline Matrix" \
      "run_performance_baseline.sh" "PASS" "$perf_pass_count"
  else
    record_result "Part 3" "Performance Baseline Matrix" \
      "run_performance_baseline.sh" "FAIL" "Performance baseline failure"
    return 1
  fi

  local leak_dir="$OUTDIR/memory-leak"
  if bash scripts/release/run_cyber_memory_leak_matrix.sh --outdir "$leak_dir"; then
    record_result "Part 3" "Valgrind Memory Leak Matrix" \
      "run_cyber_memory_leak_matrix.sh" "PASS" "No definite or indirect leaks"
  else
    record_result "Part 3" "Valgrind Memory Leak Matrix" \
      "run_cyber_memory_leak_matrix.sh" "FAIL" "Memory leak detected"
    return 1
  fi

  local record_leak_dir="$OUTDIR/memory-leak-record"
  if bash scripts/release/run_memory_leak_check.sh --outdir "$record_leak_dir"; then
    record_result "Part 3" "Valgrind Record Leak Check" \
      "run_memory_leak_check.sh" "PASS" "No definite or indirect record-engine leaks"
  else
    record_result "Part 3" "Valgrind Record Leak Check" \
      "run_memory_leak_check.sh" "FAIL" "Record memory leak detected"
    return 1
  fi
}

case "$STAGE" in
  all)
    run_part1_packaging
    run_part2_runtime
    run_part3_perf_and_stress
    ;;
  1|packaging) run_part1_packaging ;;
  2|runtime) run_part2_runtime ;;
  3|perf|stress) run_part3_perf_and_stress ;;
esac

echo "Release acceptance qualification completed"
