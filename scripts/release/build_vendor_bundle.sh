#!/usr/bin/env bash
set -euo pipefail

OUTDIR="${OUTDIR:-artifacts/vendor}"
DISTDIR="${DISTDIR:-/tmp/cache/}"
BAZEL_REGISTRY_URL="${BAZEL_REGISTRY_URL:-https://bcr.bazel.build}"
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
  echo "Refreshing the Bazel lockfile before vendoring..."
  bazel mod deps \
    --registry="$BAZEL_REGISTRY_URL" \
    --config=ci \
    --lockfile_mode=update

  echo "Materializing the vendor tree at $VENDOR_DIR..."
  rm -rf "$VENDOR_DIR"
  mkdir -p "$VENDOR_DIR"
  bazel vendor \
    --registry="$BAZEL_REGISTRY_URL" \
    --config=ci \
    --distdir="$DISTDIR" \
    --lockfile_mode=update \
    --vendor_dir="$VENDOR_DIR" \
    //:wheelos_core

  echo "Validating the vendor-mode Bazel configuration..."
  bazel build \
    --registry="$BAZEL_REGISTRY_URL" \
    --config=ci \
    --distdir="$DISTDIR" \
    --lockfile_mode=error \
    --vendor_dir="$VENDOR_DIR" \
    //:wheelos_core
fi

if [ ! -f "$REPO_ROOT/MODULE.bazel.lock" ]; then
  echo "MODULE.bazel.lock is required for offline registry resolution." >&2
  exit 1
fi
if [ ! -d "$VENDOR_DIR/_registries" ] ||
    ! find "$VENDOR_DIR/_registries" -type f -print -quit | grep -q .; then
  echo "Vendored registry files are missing from $VENDOR_DIR/_registries." >&2
  exit 1
fi

STAGING_DIR="$(mktemp -d "${TMPDIR:-/tmp}/wheelos_core_vendor.XXXXXX")"
trap 'rm -rf "$STAGING_DIR"' EXIT

mkdir -p "$STAGING_DIR/repo/vendor/bazel"

tar \
  -C "$REPO_ROOT" \
  --exclude=.git \
  --exclude=.bazelrc.user \
  --exclude='bazel-*' \
  --exclude=artifacts \
  --exclude=vendor \
  -cf - . | tar -C "$STAGING_DIR/repo" -xf -

cp -a "$VENDOR_DIR"/. "$STAGING_DIR/repo/vendor/bazel/"
rm -rf "$STAGING_DIR/repo/vendor/bazel/bazel-external"
rm -f "$STAGING_DIR/repo/.bazelrc.user"

if [ "$SKIP_PACKAGE" = false ]; then
  ARCHIVE_NAME="wheelos_core_vendor_$(date -u +%Y%m%dT%H%M%SZ).tar.gz"
  tar -C "$STAGING_DIR" -czf "$OUTDIR/$ARCHIVE_NAME" repo
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
