#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel)"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "${WORK_DIR}"' EXIT

cat > "${WORK_DIR}/MODULE.bazel" <<'EOF'
module(name = "wheelos_core_sdk_consumer", version = "0.0.0")

bazel_dep(name = "wheelos_core", version = "1.0.0")
EOF

cat > "${WORK_DIR}/BUILD.bazel" <<'EOF'
load("@rules_cc//cc:defs.bzl", "cc_binary")
load("@rules_python//python:defs.bzl", "py_binary")

cc_binary(
    name = "cpp_consumer",
    srcs = ["cpp_consumer.cc"],
    deps = ["@wheelos_core//cyber:cyber"],
)

py_binary(
    name = "python_consumer",
    srcs = ["python_consumer.py"],
    deps = ["@wheelos_core//cyber/python/cyber_py3:cyber"],
)
EOF

cat > "${WORK_DIR}/cpp_consumer.cc" <<'EOF'
#include "cyber/cyber.h"

int main() {
  return 0;
}
EOF

cat > "${WORK_DIR}/python_consumer.py" <<'EOF'
from cyber.python.cyber_py3 import cyber

assert cyber is not None
EOF

if [ -f "${REPO_ROOT}/.bazelrc" ]; then
  cp "${REPO_ROOT}/.bazelrc" "${WORK_DIR}/.bazelrc"
fi
if [ -f "${REPO_ROOT}/.bazelrc.user" ]; then
  cp "${REPO_ROOT}/.bazelrc.user" "${WORK_DIR}/.bazelrc.user"
fi

(
  cd "${WORK_DIR}"
  bazel build \
    --override_module=wheelos_core="${REPO_ROOT}" \
    //:cpp_consumer \
    //:python_consumer \
    @wheelos_core//cyber:runtime_tools
  bazel run \
    --override_module=wheelos_core="${REPO_ROOT}" \
    @wheelos_core//cyber/tools/cyber_launch:cyber_launch -- --help
)
