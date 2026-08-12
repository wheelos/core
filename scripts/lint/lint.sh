#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
source "${ROOT_DIR}/scripts/deps/installer_base.sh"

PYTHON_LINT_FLAG=0
CPP_LINT_FLAG=0

function print_usage() {
  echo "Usage: $0 [Options]"
  echo "Options:"
  echo "  --py        Lint Python files"
  echo "  --cpp       Lint C++/BUILD files"
  echo "  -a|--all    Lint all supported C++ and Python files"
  echo "  -h|--help   Show this message and exit"
}

function run_cpp_lint() {
  if command -v bazel >/dev/null 2>&1; then
    bazel test --config=ci //cyber/...
  else
    info "bazel not installed; skipping Bazel test step."
  fi

  if command -v buildifier >/dev/null 2>&1; then
    find "${ROOT_DIR}" \( -path "${ROOT_DIR}/.git" -o -path "${ROOT_DIR}/bazel-*" \) -prune -o \
      \( -name BUILD -o -name '*.bazel' -o -name '*.bzl' \) -type f -print0 \
      | xargs -0 -r buildifier -lint=fix
  else
    info "buildifier not installed; skipping BUILD formatting lint."
  fi
}

function run_py_lint() {
  if ! command -v flake8 >/dev/null 2>&1; then
    warning "Command 'flake8' not found. Install it via: python3 -m pip install flake8"
    exit 1
  fi

  find "${ROOT_DIR}" \( -path "${ROOT_DIR}/.git" -o -path "${ROOT_DIR}/bazel-*" \) -prune -o \
    -type f -name '*.py' -print0 \
    | xargs -0 -r flake8
}

function parse_cmdline_args() {
  if [[ "$#" -eq 0 ]]; then
    CPP_LINT_FLAG=1
    PYTHON_LINT_FLAG=1
    return 0
  fi

  while [[ "$#" -gt 0 ]]; do
    case "$1" in
      --py)
        PYTHON_LINT_FLAG=1
        ;;
      --cpp)
        CPP_LINT_FLAG=1
        ;;
      -a|--all)
        PYTHON_LINT_FLAG=1
        CPP_LINT_FLAG=1
        ;;
      -h|--help)
        print_usage
        exit 0
        ;;
      *)
        warning "Unknown option: $1"
        print_usage
        exit 1
        ;;
    esac
    shift
  done
}

function main() {
  parse_cmdline_args "$@"
  [[ "${CPP_LINT_FLAG}" -eq 1 ]] && run_cpp_lint
  [[ "${PYTHON_LINT_FLAG}" -eq 1 ]] && run_py_lint
}

main "$@"
