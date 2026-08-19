#!/usr/bin/env bash
set -euo pipefail

OUTDIR="artifacts/memory-leak/matrix-$(date -u +%Y%m%dT%H%M%SZ)"

while [ "$#" -gt 0 ]; do
  case "$1" in
    --outdir) OUTDIR="$2"; shift 2 ;;
    -h|--help)
      echo "Usage: $0 [--outdir DIR] or $0 [DIR]"
      exit 0
      ;;
    *)
      OUTDIR="$1"; shift ;;
  esac
done

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "${REPO_ROOT}"

if ! command -v valgrind >/dev/null 2>&1; then
  echo "valgrind is required for the memory leak matrix" >&2
  exit 2
fi

mkdir -p "${OUTDIR}"
targets=(
  "//tests/integration_test:examples_integration_test"
  "//tests/integration_test:examples_stress_test"
  "//cyber/parameter:parameter_client_test"
  "//cyber/parameter:parameter_server_test"
  "//cyber/parameter:parameter_test"
)

failed=0
bazel build --config=ci "${targets[@]}" >/dev/null
for target in "${targets[@]}"; do
  name="${target#//}"
  name="${name//\//_}"
  name="${name//:/_}"
  log="${OUTDIR}/${name}.log"
  valgrind_log="${OUTDIR}/${name}.valgrind.log"
  package="${target#//}"
  package="${package%%:*}"
  binary="${target##*:}"
  echo "Running ${target}"
  set +e
  valgrind \
    --leak-check=full \
    --show-leak-kinds=definite,possible \
    --errors-for-leak-kinds=definite,indirect \
    --undef-value-errors=no \
    --log-file="${REPO_ROOT}/${valgrind_log}" \
    --error-exitcode=1 \
    "${REPO_ROOT}/bazel-bin/${package}/${binary}" >"${log}" 2>&1
  status=$?
  set -e
  {
    echo "target=${target}"
    echo "exit_code=${status}"
    if [ -f "${valgrind_log}" ]; then
      grep -E "definitely lost:|indirectly lost:|possibly lost:|still reachable:|ERROR SUMMARY:" \
        "${valgrind_log}" || true
    fi
  } > "${OUTDIR}/${name}.summary"
  if [ "${status}" -ne 0 ]; then
    failed=1
  fi
done

{
  echo "generated_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "git_sha=$(git rev-parse HEAD)"
  echo "target_count=${#targets[@]}"
  echo "failed=${failed}"
} > "${OUTDIR}/metadata.txt"

if [ "${failed}" -ne 0 ]; then
  echo "Cyber memory leak matrix found failures; reports=${OUTDIR}" >&2
  exit 1
fi
echo "Cyber memory leak matrix passed; reports=${OUTDIR}"
