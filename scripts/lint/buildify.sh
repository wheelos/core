#!/bin/bash

set -e

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
echo $ROOT_DIR
source "${ROOT_DIR}/scripts/deps/installer_base.sh"

SYSROOT_BIN_DIR="${SYSROOT_DIR}/bin"

function buildify() {
  local buildifier_cmd="${SYSROOT_BIN_DIR}/buildifier"
  if [ ! -f "${buildifier_cmd}" ]; then
    error "Command buildifier not found."
    exit 1
  fi

  buildifier_cmd="${buildifier_cmd} -lint=fix"

  local build_dirs=(
    "cyber"
    "bazel"
    "tools"
  )

  set -x
  find "${build_dirs[@]/#/${ROOT_DIR}/}" -type f \
    \( -name "BUILD" -or -name "*.BUILD" -or -name "*.bzl" -or -name "*.bazel" \) \
    -exec ${buildifier_cmd} {} +
  set +x

  echo "buildifier run finished successfully."

  local files=(
    # Todo(zero): need fix
    #"${ROOT_DIR}/.bazelrc"
    "${ROOT_DIR}/BUILD"
    "${ROOT_DIR}/MODULE.bazel"
    "${ROOT_DIR}/WORKSPACE"
  )

  for file in "${files[@]}"; do
    if [ -f "${file}" ]; then
      ${buildifier_cmd} "${file}"
    fi
  done
}

function main() {
  buildify "$@"
}

main "$@"
