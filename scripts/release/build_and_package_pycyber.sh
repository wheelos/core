#!/usr/bin/env bash
set -euo pipefail

# Build and package pycyber, run auditwheel repair and smoke validation.
# After success, artifacts are placed in packaging/pycyber/wheelhouse.
# Usage: ./scripts/release/build_and_package_pycyber.sh [--skip-auditwheel] [--skip-validate] [--skip-sdist] [--skip-build] [--outdir DIR]

SKIP_AUDITWHEEL=false
SKIP_VALIDATE=false
SKIP_SDIST=false
SKIP_BUILD=false
OUTDIR="packaging/pycyber/wheelhouse"

while [ $# -gt 0 ]; do
  case "$1" in
    --skip-auditwheel) SKIP_AUDITWHEEL=true; shift ;;
    --skip-validate) SKIP_VALIDATE=true; shift ;;
    --skip-sdist) SKIP_SDIST=true; shift ;;
    --skip-build) SKIP_BUILD=true; shift ;;
    --outdir) OUTDIR="$2"; shift 2 ;;
    -h|--help) echo "Usage: $0 [--skip-auditwheel] [--skip-validate] [--skip-sdist] [--skip-build] [--outdir DIR]"; exit 0 ;;
    *) echo "Unknown arg: $1"; exit 1 ;;
  esac
done

echo "Starting pycyber build+package at $(date -u +%Y-%m-%dT%H:%M:%SZ)"

REPO_ROOT=$(git rev-parse --show-toplevel)
cd "$REPO_ROOT"

PYTHON_BIN=${PYTHON:-python3}
PYTHON_BIN="$("$PYTHON_BIN" -c 'import sys; print(sys.executable)')"
VENV_DIR=packaging/pycyber/.venv
TARGET_PYTHON_VERSION="$("$PYTHON_BIN" -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')"
if ! "$PYTHON_BIN" -c 'import sys; raise SystemExit(0 if sys.version_info >= (3, 8) else 1)'; then
  echo "Error: pycyber release requires Python >= 3.8, got $TARGET_PYTHON_VERSION from $PYTHON_BIN" >&2
  exit 1
fi
if [ -z "${PYTHON:-}" ] && [ "$TARGET_PYTHON_VERSION" != "3.10" ]; then
  BAZEL_EXECROOT="$(bazel info execution_root 2>/dev/null || true)"
  for candidate in "$BAZEL_EXECROOT"/external/rules_python~~python~python_3_10_*/bin/python3; do
    if [ -x "$candidate" ]; then
      PYTHON_BIN="$candidate"
      TARGET_PYTHON_VERSION="$("$PYTHON_BIN" -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')"
      echo "Using Bazel-managed Python $TARGET_PYTHON_VERSION interpreter: $PYTHON_BIN"
      break
    fi
  done
fi
if [ "$TARGET_PYTHON_VERSION" != "3.10" ]; then
  echo "Error: pycyber packaging currently requires Python 3.10, got $TARGET_PYTHON_VERSION from $PYTHON_BIN" >&2
  exit 1
fi
if [ -x "$VENV_DIR/bin/python" ]; then
  if ! "$VENV_DIR/bin/python" -c 'import sys; print(sys.executable)' >/dev/null 2>&1; then
    rm -rf "$VENV_DIR"
  else
    VENV_PYTHON_VERSION="$("$VENV_DIR/bin/python" -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')"
    if [ "$VENV_PYTHON_VERSION" != "$TARGET_PYTHON_VERSION" ]; then
      rm -rf "$VENV_DIR"
    fi
  fi
fi
if [ ! -x "$VENV_DIR/bin/python" ]; then
  echo "Creating pycyber build virtual environment at $VENV_DIR"
    rm -rf "$VENV_DIR"
    "$PYTHON_BIN" -m venv "$VENV_DIR"
fi
if ! "$VENV_DIR/bin/python" -c 'import sys; print(sys.executable)' >/dev/null 2>&1; then
    rm -rf "$VENV_DIR"
    echo "Recreating pycyber build virtual environment at $VENV_DIR"
    "$PYTHON_BIN" -m venv "$VENV_DIR"
fi
PYTHON_BIN="$REPO_ROOT/$VENV_DIR/bin/python"
echo "Using Python interpreter: $PYTHON_BIN"

echo "Fetching tags from origin..."
git fetch --tags --prune origin || true

# Keep Python artifacts on the same release line as the Bzlmod module and
# native runtime. Untagged commits are deliberately marked as prereleases.
CORE_VERSION="$(sed -nE 's/^[[:space:]]*version = "([^"]+)".*/\1/p' MODULE.bazel | head -n 1)"
if [ -z "$CORE_VERSION" ]; then
  echo "Unable to determine wheelos_core version from MODULE.bazel" >&2
  exit 1
fi

if [ -n "${PYCYBER_VERSION:-}" ]; then
  VERSION="$PYCYBER_VERSION"
  TAG="PYCYBER_VERSION"
elif git describe --exact-match --tags --match "wheelos_core-v${CORE_VERSION}" >/dev/null 2>&1; then
  VERSION="$CORE_VERSION"
  TAG="wheelos_core-v${CORE_VERSION}"
else
  TAG="N/A"
  VERSION="${CORE_VERSION}.dev$(git rev-list --count HEAD)+g$(git rev-parse --short HEAD)"
fi
echo "Detected version: $VERSION (tag: ${TAG:-N/A})"
export SETUPTOOLS_SCM_PRETEND_VERSION="$VERSION"

# Ensure Python build tools are available
echo "Installing/ensuring Python build tools (build, auditwheel, twine, setuptools_scm)..."
"$PYTHON_BIN" -m pip install --upgrade pip
"$PYTHON_BIN" -m pip install --upgrade build auditwheel twine setuptools_scm

# Check patchelf on Linux for auditwheel
if [ "$(uname -s)" = "Linux" ]; then
  if ! command -v patchelf >/dev/null 2>&1; then
    echo "Warning: patchelf not found. Install it (e.g. apt-get install -y patchelf) for auditwheel to patch RPATHs." >&2
  fi
fi

STAGING_DIR=packaging/pycyber/staging
BUILD_DIR=packaging/pycyber/build
DIST_DIR=packaging/pycyber/dist
WHEELHOUSE_DIR="$OUTDIR"

# Clean previous outputs. --skip-build intentionally reuses an existing staged
# package, so it must not remove STAGING_DIR.
if [ "$SKIP_BUILD" = false ]; then
  rm -rf "$STAGING_DIR"
fi
rm -rf "$BUILD_DIR" "$DIST_DIR" "$WHEELHOUSE_DIR"
mkdir -p "$DIST_DIR" "$WHEELHOUSE_DIR"

# Stage package (generates staging/src/pycyber)
if [ "$SKIP_BUILD" = false ]; then
  echo "Staging pycyber package (this may run bazel build)..."
  "$PYTHON_BIN" scripts/release/build_pycyber.py || { echo "Staging failed" >&2; exit 1; }
else
  echo "Skipping staging (--skip-build)"
fi

# Build wheel (and sdist unless skipped)
BUILD_ARGS=(--wheel)
if [ "$SKIP_SDIST" = false ]; then
  BUILD_ARGS=(--sdist --wheel)
fi

echo "Running: $PYTHON_BIN -m build ${BUILD_ARGS[*]} --outdir $DIST_DIR packaging/pycyber"
"$PYTHON_BIN" -m build "${BUILD_ARGS[@]}" --outdir "$DIST_DIR" packaging/pycyber || { echo "Build failed" >&2; exit 1; }

shopt -s nullglob
DIST_WHEELS=("$DIST_DIR"/*.whl)
if [ ${#DIST_WHEELS[@]} -eq 0 ]; then
  echo "No wheel was produced in $DIST_DIR" >&2
  exit 1
fi

# Repair wheels with auditwheel on Linux
if [ "$SKIP_AUDITWHEEL" = false ] && [ "$(uname -s)" = "Linux" ]; then
  echo "Running auditwheel repair for Linux wheels..."
  for whl in "${DIST_WHEELS[@]}"; do
    echo "Repairing $whl"
    "$PYTHON_BIN" -m auditwheel repair "$whl" -w "$WHEELHOUSE_DIR" || { echo "auditwheel repair failed for $whl" >&2; exit 1; }
  done
  REPAIRED_WHEELS=("$WHEELHOUSE_DIR"/*.whl)
  if [ ${#REPAIRED_WHEELS[@]} -eq 0 ]; then
    echo "No wheels were repaired; copying built wheels to wheelhouse"
    cp "${DIST_WHEELS[@]}" "$WHEELHOUSE_DIR"/
  fi
else
  echo "Skipping auditwheel (not requested or not available). Copying built wheels to wheelhouse"
  cp "${DIST_WHEELS[@]}" "$WHEELHOUSE_DIR"/
fi

# Copy sdist to wheelhouse if present
DIST_SDISTS=("$DIST_DIR"/*.tar.gz)
if [ ${#DIST_SDISTS[@]} -gt 0 ]; then
  cp "${DIST_SDISTS[@]}" "$WHEELHOUSE_DIR"/
fi

# Collect distributable artifacts only (exclude SHA256SUMS and other side files).
DIST_UPLOADS=("$WHEELHOUSE_DIR"/*.whl "$WHEELHOUSE_DIR"/*.tar.gz)
if [ ${#DIST_UPLOADS[@]} -eq 0 ]; then
  echo "No distribution artifacts found in $WHEELHOUSE_DIR" >&2
  exit 1
fi

# Run twine check
echo "Running twine check on artifacts..."
"$PYTHON_BIN" -m twine check "${DIST_UPLOADS[@]}" || { echo "twine check failed" >&2; exit 1; }

# Run smoke validation using the provided validation script
if [ "$SKIP_VALIDATE" = false ]; then
  WHEELS=( "$WHEELHOUSE_DIR"/*.whl )
  if [ ${#WHEELS[@]} -eq 0 ]; then
    echo "No wheel found in $WHEELHOUSE_DIR to validate" >&2
    exit 1
  fi
  VALIDATE_WHEEL="${WHEELS[0]}"
  echo "Validating wheel: $VALIDATE_WHEEL"
  "$PYTHON_BIN" scripts/release/validate_pycyber.py --python "$PYTHON_BIN" --wheel "$VALIDATE_WHEEL" || { echo "Validation failed" >&2; exit 1; }
  echo "Running pycyber packaging example coverage..."
  "$PYTHON_BIN" packaging/pycyber/verify_pycyber_examples.py --python "$PYTHON_BIN" --wheel "$VALIDATE_WHEEL" || { echo "Packaging example verification failed" >&2; exit 1; }
  echo "Running pycyber example smoke tests via Bazel..."
  bazel test --config=ci //cyber/python/cyber_py3/examples:examples_smoke_test --test_output=errors || { echo "Example smoke test failed" >&2; exit 1; }
else
  echo "Skipping validation (--skip-validate)"
fi

# Create SHA256SUMS
if command -v sha256sum >/dev/null 2>&1; then
  echo "Creating SHA256SUMS in $WHEELHOUSE_DIR"
  (cd "$WHEELHOUSE_DIR" && sha256sum * > SHA256SUMS) || true
else
  echo "sha256sum not found - skipping SHA256SUMS generation"
fi

echo "Build and validation finished successfully. Artifacts are in: $WHEELHOUSE_DIR"
echo "To upload to PyPI: $PYTHON_BIN -m twine upload $WHEELHOUSE_DIR/*.whl $WHEELHOUSE_DIR/*.tar.gz"
echo "To upload to TestPyPI: $PYTHON_BIN -m twine upload --repository testpypi $WHEELHOUSE_DIR/*.whl $WHEELHOUSE_DIR/*.tar.gz"

exit 0
