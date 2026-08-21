#!/usr/bin/env bash
set -euo pipefail

MODE="check"

while [ "$#" -gt 0 ]; do
  case "$1" in
    --check) MODE="check"; shift ;;
    --update) MODE="update"; shift ;;
    -h|--help)
      echo "Usage: $0 [--check|--update]"
      exit 0
      ;;
    *)
      echo "Unknown arg: $1" >&2
      exit 1
      ;;
  esac
done

BAZEL_REGISTRY_URL="${BAZEL_REGISTRY_URL:-https://bcr.bazel.build}"
REPO_ROOT=$(git rev-parse --show-toplevel)
cd "$REPO_ROOT"

LOCKFILE_TARGETS=(
  //cyber
  //cyber:runtime_tools
  //:wheelos_core
  //cyber/message/...
  //cyber/python/internal:_cyber_wrapper.so
  //cyber/python/cyber_py3/test:all
  //cyber/python/cyber_py3/examples:examples_smoke_test
  //cyber/transport/rtps:rtps_test
  //cyber/transport/integration_test:rtps_transceiver_test
  //tests/integration_test:examples_regression_tests
  //cyber/proto:record_py_pb2
)

if [ "$MODE" = "check" ] && [ ! -f MODULE.bazel.lock ]; then
  echo "MODULE.bazel.lock is missing. Run: bash scripts/release/check_bzlmod_lockfile.sh --update" >&2
  exit 1
fi

if [ "$MODE" = "update" ]; then
  bazel build --registry="$BAZEL_REGISTRY_URL" --nobuild --config=ci --config=lockfile-update "${LOCKFILE_TARGETS[@]}" >/dev/null
  echo "Updated MODULE.bazel.lock"
else
  bazel build --registry="$BAZEL_REGISTRY_URL" --nobuild --config=ci --config=lockfile-check "${LOCKFILE_TARGETS[@]}" >/dev/null
  echo "MODULE.bazel.lock is in sync"
fi
