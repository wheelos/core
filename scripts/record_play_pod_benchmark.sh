#!/usr/bin/env bash
set -euo pipefail

SOURCE="${1:-/mnt/synology/apollo/sensor_rgb.record}"
OUTPUT="${2:-/mnt/synology/apollo/sensor_rgb_pod.record}"
MANIFEST="${3:-/mnt/synology/apollo/sensor_rgb_pod.manifest.tsv}"
MAX_PER_CHANNEL="${4:-0}"
MODE="${5:-benchmark}"

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "${REPO_ROOT}"

if [ ! -s "${SOURCE}" ]; then
  echo "record file is missing or empty: ${SOURCE}" >&2
  exit 2
fi

bazel build --config=ci //examples/record_play:record_play_tool_example
exec "${REPO_ROOT}/bazel-bin/examples/record_play/record_play_tool_example" \
  --mode="${MODE}" \
  --source="${SOURCE}" \
  --output="${OUTPUT}" \
  --manifest="${MANIFEST}" \
  --max_per_channel="${MAX_PER_CHANNEL}"
