#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "${REPO_ROOT}"

FILE_PATH="${1:-/mnt/synology/apollo/sensor_rgb.record}"
MAX_CHUNKS="${2:-0}"
OUTDIR="${3:-artifacts/performance/record-reader-$(date -u +%Y%m%dT%H%M%SZ)}"

bazel build //cyber/record:record_perf_reader >/dev/null
mkdir -p "${OUTDIR}"

run_mode() {
  local mode="$1"
  shift || true
  ./bazel-bin/cyber/record/record_perf_reader \
    --file="${FILE_PATH}" \
    --mode="${mode}" \
    --max_chunks="${MAX_CHUNKS}" \
    "$@"
}

echo "# record_perf_reader begin file=${FILE_PATH} max_chunks=${MAX_CHUNKS}" | tee "${OUTDIR}/benchmark.log"
for mode in baseline uring uring_stream; do
  echo "## mode=${mode}" | tee -a "${OUTDIR}/benchmark.log"
  run_mode "${mode}" | tee "${OUTDIR}/${mode}.json" | tee -a "${OUTDIR}/benchmark.log"
done
echo "## mode=uring_hysteresis" | tee -a "${OUTDIR}/benchmark.log"
run_mode uring_hysteresis --hwm_mb=500 --lwm_mb=100 --replay_rate=200 |
  tee "${OUTDIR}/uring_hysteresis.json" | tee -a "${OUTDIR}/benchmark.log"

if command -v strace >/dev/null 2>&1; then
  strace -c -o "${OUTDIR}/strace_baseline.txt" \
    ./bazel-bin/cyber/record/record_perf_reader --file="${FILE_PATH}" --mode=baseline --max_chunks="${MAX_CHUNKS}" \
    >"${OUTDIR}/strace_baseline.json"
  strace -c -o "${OUTDIR}/strace_uring_stream.txt" \
    ./bazel-bin/cyber/record/record_perf_reader --file="${FILE_PATH}" --mode=uring_stream --max_chunks="${MAX_CHUNKS}" \
    >"${OUTDIR}/strace_uring_stream.json"
  echo "# strace summaries: ${OUTDIR}/strace_baseline.txt ${OUTDIR}/strace_uring_stream.txt" |
    tee -a "${OUTDIR}/benchmark.log"
fi

{
  echo "generated_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "git_sha=$(git rev-parse HEAD)"
  echo "record_file=${FILE_PATH}"
  echo "max_chunks=${MAX_CHUNKS}"
  echo "outdir=${OUTDIR}"
} > "${OUTDIR}/metadata.txt"

echo "# record_perf_reader done; reports=${OUTDIR}"
