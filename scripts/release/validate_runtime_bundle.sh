#!/usr/bin/env bash
set -euo pipefail

DEB=""
BUILD_CONFIG=""
DISTDIR="${DISTDIR:-/tmp/cache/}"
WORKDIR=""

while [ "$#" -gt 0 ]; do
  case "$1" in
    --deb) DEB="$2"; shift 2 ;;
    --config) BUILD_CONFIG="$2"; shift 2 ;;
    --distdir) DISTDIR="$2"; shift 2 ;;
    --workdir) WORKDIR="$2"; shift 2 ;;
    -h|--help)
      echo "Usage: $0 (--deb PATH | --config NAME) [--distdir DIR] [--workdir DIR]"
      exit 0
      ;;
    *)
      echo "Unknown arg: $1" >&2
      exit 1
      ;;
  esac
done

if [ -n "$DEB" ] && [ -n "$BUILD_CONFIG" ]; then
  echo "Use exactly one of --deb or --config" >&2
  exit 1
fi
if [ -z "$DEB" ] && [ -z "$BUILD_CONFIG" ]; then
  echo "One of --deb or --config is required" >&2
  exit 1
fi

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "${REPO_ROOT}"

if [ -n "$BUILD_CONFIG" ]; then
  bazel build --config="$BUILD_CONFIG" --distdir="$DISTDIR" //:wheelos_core
  mapfile -t DEB_OUTPUTS < <(
    bazel cquery --config="$BUILD_CONFIG" --distdir="$DISTDIR" \
      //:wheelos_core --output=files |
      sed -n '/\.deb$/p'
  )
  if [ "${#DEB_OUTPUTS[@]}" -ne 1 ]; then
    echo "Expected exactly one deb output for //:wheelos_core, found ${#DEB_OUTPUTS[@]}" >&2
    exit 1
  fi
  DEB="${DEB_OUTPUTS[0]}"
fi

if [[ "$DEB" != /* ]]; then
  DEB="$REPO_ROOT/$DEB"
fi
if [ ! -f "$DEB" ]; then
  echo "Runtime bundle does not exist: $DEB" >&2
  exit 1
fi

if [ -z "$WORKDIR" ]; then
  WORKDIR="$REPO_ROOT/artifacts/runtime-bundle-validation"
elif [[ "$WORKDIR" != /* ]]; then
  WORKDIR="$REPO_ROOT/$WORKDIR"
fi
BUNDLE_ROOT="$WORKDIR/root"
rm -rf "$WORKDIR"
mkdir -p "$BUNDLE_ROOT"
trap 'rm -rf "${WORKDIR}"' EXIT

dpkg-deb -x "${DEB}" "${BUNDLE_ROOT}"
source "${BUNDLE_ROOT}/opt/wheelos_core/setup.bash"

command -v mainboard
command -v cyber_recorder
command -v cyber_monitor
command -v cyber_launch
cyber_launch --help
