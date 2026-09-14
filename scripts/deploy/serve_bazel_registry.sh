#!/usr/bin/env bash

set -euo pipefail

HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-8000}"

REPO_ROOT=$(git rev-parse --show-toplevel)
REGISTRY_DIR="${REGISTRY_DIR:-${REPO_ROOT}/data/bazel-central-registry}"

if [ ! -d "${REGISTRY_DIR}" ]; then
  echo "Registry directory not found: ${REGISTRY_DIR}" >&2
  exit 1
fi

exec python3 -m http.server "${PORT}" --bind "${HOST}" --directory "${REGISTRY_DIR}"
