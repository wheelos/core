#!/usr/bin/env bash
set -euo pipefail

OUTDIR="${OUTDIR:-artifacts/vendor}"
DISTDIR="${DISTDIR:-/tmp/cache/}"
REPO_ROOT=$(git rev-parse --show-toplevel)
VENDOR_DIR="${BAZEL_VENDOR_DIR:-$REPO_ROOT/vendor/bazel}"
SKIP_BUILD=false
SKIP_PACKAGE=false

while [ "$#" -gt 0 ]; do
  case "$1" in
    --outdir) OUTDIR="$2"; shift 2 ;;
    --distdir) DISTDIR="$2"; shift 2 ;;
    --vendor-dir) VENDOR_DIR="$2"; shift 2 ;;
    --skip-build) SKIP_BUILD=true; shift ;;
    --skip-package) SKIP_PACKAGE=true; shift ;;
    -h|--help)
      echo "Usage: $0 [--outdir DIR] [--distdir DIR] [--vendor-dir DIR] [--skip-build] [--skip-package]"
      echo ""
      echo "Creates a vendor-mode release bundle that contains the repository snapshot and a vendored Bzlmod cache."
      echo "This is a separate offline deployment mode from the default release artifact flow."
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

mkdir -p "$OUTDIR"
rm -rf "$OUTDIR"/*

if [ "$SKIP_BUILD" = false ]; then
  if [ -d "$VENDOR_DIR" ]; then
    echo "Using existing vendor tree at $VENDOR_DIR"
  else
    echo "Preparing vendor dir at $VENDOR_DIR"
    mkdir -p "$VENDOR_DIR"
  fi

  echo "Validating the vendor-mode Bazel configuration..."
  bazel build --config=ci --distdir="$DISTDIR" --vendor_dir="$VENDOR_DIR" //:wheelos_core
fi

STAGING_DIR="$(mktemp -d "${TMPDIR:-/tmp}/wheelos_core_vendor.XXXXXX")"
trap 'rm -rf "$STAGING_DIR"' EXIT

mkdir -p "$STAGING_DIR/repo" "$STAGING_DIR/vendor"

cp -a "$REPO_ROOT"/. "$STAGING_DIR/repo/"
if [ -d "$VENDOR_DIR" ]; then
  cp -a "$VENDOR_DIR"/. "$STAGING_DIR/vendor/"
else
  echo "Warning: vendor dir $VENDOR_DIR does not exist; bundling repository only. Populate it with --vendor_dir before offline builds." >&2
  printf '%s\n' "This bundle is intended for Bzlmod vendor/offline builds." > "$STAGING_DIR/vendor/README.txt"
fi

if [ "$SKIP_PACKAGE" = false ]; then
  ARCHIVE_NAME="wheelos_core_vendor_$(date -u +%Y%m%dT%H%M%SZ).tar.gz"
  tar -C "$STAGING_DIR" -czf "$OUTDIR/$ARCHIVE_NAME" repo vendor
  echo "Vendor bundle created: $OUTDIR/$ARCHIVE_NAME"
else
  echo "Vendor bundle staging is ready in $STAGING_DIR"
fi

{
  echo "generated_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "git_sha=$(git rev-parse HEAD)"
  echo "bazel_version=$(bazel --version)"
  echo "vendor_dir=$VENDOR_DIR"
  echo "archive_dir=$OUTDIR"
} > "$OUTDIR/manifest.txt"

exit 0
