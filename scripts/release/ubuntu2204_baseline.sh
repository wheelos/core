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

bash scripts/release/check_bzlmod_lockfile.sh --check

bazel build --config=ci --distdir="$DISTDIR" //cyber //:wheelos_core

bazel test --config=ci --distdir="$DISTDIR" \
  //cyber/... \
  //tests/integration_test:examples_regression_tests

if [ "$RUN_PYCYBER" = true ]; then
  bash scripts/release/build_and_package_pycyber.sh
fi

echo "Ubuntu 22.04 baseline completed successfully"
