#!/usr/bin/env bash
set -euo pipefail

OUTDIR="artifacts/memory-leak/$(date -u +%Y%m%dT%H%M%SZ)"
TARGET="//examples:record"
RECORD_FILE=""
MODE="uring_stream"
MAX_CHUNKS="1"

while [ "$#" -gt 0 ]; do
  case "$1" in
    --outdir) OUTDIR="$2"; shift 2 ;;
    --record-file) RECORD_FILE="$2"; shift 2 ;;
    --mode) MODE="$2"; shift 2 ;;
    --max-chunks) MAX_CHUNKS="$2"; shift 2 ;;
    -h|--help)
      echo "Usage: $0 [--outdir DIR] [--record-file PATH] [--mode MODE] [--max-chunks N]"
      exit 0
      ;;
    *) echo "Unknown arg: $1" >&2; exit 2 ;;
  esac
done

if ! command -v valgrind >/dev/null 2>&1; then
  echo "valgrind is required for the memory leak check" >&2
  exit 2
fi

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "${REPO_ROOT}"
mkdir -p "${OUTDIR}"

if [ -n "${RECORD_FILE}" ]; then
  if [ ! -s "${RECORD_FILE}" ]; then
    echo "Record file is missing or empty: ${RECORD_FILE}" >&2
    exit 2
  fi
  TARGET="//cyber/record:record_perf_reader"
  bazel build --config=ci "${TARGET}" >/dev/null
  COMMAND=(
    "${REPO_ROOT}/bazel-bin/cyber/record/record_perf_reader"
    "--file=${RECORD_FILE}"
    "--mode=${MODE}"
    "--max_chunks=${MAX_CHUNKS}"
  )
else
  case "${TARGET}" in
    //examples:record)
    bazel build --config=ci "${TARGET}" >/dev/null
    COMMAND=("${REPO_ROOT}/bazel-bin/examples/record")
    ;;
    *) echo "Unsupported target: ${TARGET}" >&2; exit 2 ;;
  esac
fi

valgrind \
  --leak-check=full \
  --show-leak-kinds=definite,possible \
  --errors-for-leak-kinds=definite,indirect \
  --error-exitcode=1 \
  --undef-value-errors=no \
  --log-file="${OUTDIR}/valgrind.log" \
  "${COMMAND[@]}"

{
  echo "generated_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "git_sha=$(git rev-parse HEAD)"
  echo "target=${TARGET}"
  echo "record_file=${RECORD_FILE}"
  echo "mode=${MODE}"
  echo "max_chunks=${MAX_CHUNKS}"
  echo "result=clean"
} > "${OUTDIR}/metadata.txt"
echo "Memory leak check passed; report=${OUTDIR}/valgrind.log"
