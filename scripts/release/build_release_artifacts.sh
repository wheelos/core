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

CORE_PACKAGE_BUILT=false
if [ "$SKIP_BASELINE" = false ]; then
  bash scripts/release/ubuntu2204_baseline.sh --distdir "$DISTDIR"
  CORE_PACKAGE_BUILT=true
fi

rm -rf "$OUTDIR"
mkdir -p "$OUTDIR/core" "$OUTDIR/pycyber"

if [ "$SKIP_CORE_PACKAGE" = false ]; then
  if [ "$CORE_PACKAGE_BUILT" = false ]; then
    bazel build --config=ci --distdir="$DISTDIR" //:wheelos_core
  fi
  mapfile -t CORE_OUTPUTS < <(
    bazel cquery --config=ci --distdir="$DISTDIR" \
      //:wheelos_core --output=files |
      sed -n '/\.deb$/p'
  )

  if [ "${#CORE_OUTPUTS[@]}" -ne 1 ]; then
    echo "Expected exactly one deb output for //:wheelos_core, found ${#CORE_OUTPUTS[@]}" >&2
    exit 1
  fi

  CORE_DEB="${CORE_OUTPUTS[0]}"
  if [[ "$CORE_DEB" != /* ]]; then
    CORE_DEB="$REPO_ROOT/$CORE_DEB"
  fi
  if [ ! -f "$CORE_DEB" ]; then
    echo "Configured //:wheelos_core output does not exist: $CORE_DEB" >&2
    exit 1
  fi

  bash scripts/release/validate_runtime_bundle.sh \
    --deb "$CORE_DEB" \
    --workdir "$OUTDIR/.runtime-bundle-validation"
  cp -f "$CORE_DEB" "$OUTDIR/core/$(basename "$CORE_DEB")"
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

# Keep checksums for every distributable artifact, including the native package.
(
  cd "$OUTDIR"
  find core pycyber -type f ! -name 'SHA256SUMS' -print0 |
    sort -z |
    xargs -0 sha256sum
) > "$OUTDIR/SHA256SUMS"

echo "Release artifacts are available in $OUTDIR"
