#!/usr/bin/env bash
set -euo pipefail

DISTDIR="${DISTDIR:-/tmp/cache/}"
RUN_PYCYBER=false

while [ "$#" -gt 0 ]; do
  case "$1" in
    --distdir) DISTDIR="$2"; shift 2 ;;
    --with-pycyber) RUN_PYCYBER=true; shift ;;
    -h|--help)
      echo "Usage: $0 [--distdir DIR] [--with-pycyber]"
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

python3 scripts/release/check_release_version.py --release
bash scripts/release/check_bzlmod_lockfile.sh --check

bazel build --config=ci --distdir="$DISTDIR" //cyber //:wheelos_core

TEST_ENV_ARGS=()
if [[ -v CYBER_RECORD_PLAY_FIXTURE ]]; then
  TEST_ENV_ARGS+=("--test_env=CYBER_RECORD_PLAY_FIXTURE=$CYBER_RECORD_PLAY_FIXTURE")
fi
if [[ -n "${PYTHONUSERBASE:-}" ]]; then
  TEST_ENV_ARGS+=("--test_env=PYTHONUSERBASE=$PYTHONUSERBASE")
fi
bazel test --config=ci --distdir="$DISTDIR" \
  "${TEST_ENV_ARGS[@]}" \
  //scripts/release:release_scripts_test \
  //cyber/node:writer_test \
  //cyber/node:writer_reader_test \
  //cyber/data:all_latest_test \
  //cyber/data:channel_buffer_test \
  //cyber/data:data_visitor_test \
  //cyber/component:component_test \
  //cyber/metrics:metrics_test \
  //tests/perf_test:runtime_metrics_hot_path_benchmark_test \
  //tests/perf_test:runtime_metrics_pubsub_benchmark_test \
  //tests/perf_test:runtime_metrics_compare_test \
  //tests/perf_test:runtime_metrics_snapshot_benchmark_test \
  //tests/integration_test:core_tool_matrix_tests

if [ "$RUN_PYCYBER" = true ]; then
  bash scripts/release/build_and_package_pycyber.sh
fi

echo "Ubuntu 22.04 baseline completed successfully"
