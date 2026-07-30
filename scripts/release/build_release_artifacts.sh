#!/usr/bin/env bash
set -euo pipefail

OUTDIR="artifacts/release"
DISTDIR="${DISTDIR:-/tmp/cache/}"
SKIP_BASELINE=false
SKIP_CORE_PACKAGE=false
SKIP_PYCYBER=false
SKIP_AUDITWHEEL=false

while [ "$#" -gt 0 ]; do
  case "$1" in
    --outdir) OUTDIR="$2"; shift 2 ;;
    --distdir) DISTDIR="$2"; shift 2 ;;
    --skip-baseline) SKIP_BASELINE=true; shift ;;
    --skip-core-package) SKIP_CORE_PACKAGE=true; shift ;;
    --skip-pycyber) SKIP_PYCYBER=true; shift ;;
    --skip-auditwheel) SKIP_AUDITWHEEL=true; shift ;;
    -h|--help)
      echo "Usage: $0 [--outdir DIR] [--distdir DIR] [--skip-baseline] [--skip-core-package] [--skip-pycyber] [--skip-auditwheel]"
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

if [ "$SKIP_BASELINE" = false ]; then
  bash scripts/release/ubuntu2204_baseline.sh --distdir "$DISTDIR"
fi

rm -rf "$OUTDIR"
mkdir -p "$OUTDIR/core" "$OUTDIR/pycyber"

if [ "$SKIP_CORE_PACKAGE" = false ]; then
  bazel build --config=ci --distdir="$DISTDIR" //:wheelos_core
  bash scripts/release/validate_runtime_bundle.sh
  mapfile -t CORE_OUTPUTS < <(
    bazel cquery //:wheelos_core \
      --output=starlark \
      --starlark:expr='"\n".join([f.path for f in target.files.to_list()])' |
      sed '/^$/d'
  )

  if [ "${#CORE_OUTPUTS[@]}" -eq 0 ]; then
    echo "No outputs found for //:wheelos_core" >&2
    exit 1
  fi

  for output in "${CORE_OUTPUTS[@]}"; do
    cp -f "$REPO_ROOT/$output" "$OUTDIR/core/$(basename "$output")"
  done
fi

if [ "$SKIP_PYCYBER" = false ]; then
  PYCYBER_ARGS=(--outdir "$OUTDIR/pycyber")
  if [ "$SKIP_AUDITWHEEL" = true ]; then
    PYCYBER_ARGS+=(--skip-auditwheel)
  fi
  bash scripts/release/build_and_package_pycyber.sh "${PYCYBER_ARGS[@]}"
fi

{
  echo "generated_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "git_sha=$(git rev-parse HEAD)"
  echo "bazel_version=$(bazel --version)"
  echo "core_artifacts:"
  find "$OUTDIR/core" -maxdepth 1 -type f -printf '  %f\n' | sort
  echo "pycyber_artifacts:"
  find "$OUTDIR/pycyber" -maxdepth 1 -type f -printf '  %f\n' | sort
} > "$OUTDIR/manifest.txt"

echo "Release artifacts are available in $OUTDIR"
