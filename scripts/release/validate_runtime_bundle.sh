#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "${REPO_ROOT}"

bazel build //:wheelos_core
DEB="$(bazel cquery //:wheelos_core --output=files | tail -n 1)"
BUNDLE_ROOT="$(mktemp -d)"
trap 'rm -rf "${BUNDLE_ROOT}"' EXIT

dpkg-deb -x "${DEB}" "${BUNDLE_ROOT}"
source "${BUNDLE_ROOT}/opt/wheelos_core/setup.bash"

command -v mainboard
command -v cyber_recorder
command -v cyber_monitor
command -v cyber_launch
cyber_launch --help
