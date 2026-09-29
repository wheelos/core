#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
cd "${ROOT_DIR}"

CPP_LINT_FLAG=0
PYTHON_LINT_FLAG=0
SHELL_LINT_FLAG=0
DIFF_BASE=""

print_usage() {
  cat <<EOF
Usage: $0 [Options]
Options:
  --cpp             Check C++ formatting and Bazel formatting/lint
  --py              Check Python formatting and lint
  --sh              Check shell scripts with ShellCheck
  --diff <commit>   Check only files changed from <commit> to the working tree
  -a|--all          Run all supported checks (default)
  -h|--help         Show this message and exit
EOF
}

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Required command '$1' was not found." >&2
    return 1
  fi
}

collect_files() {
  local pattern="$1"
  local result_name="$2"
  local -n result="${result_name}"
  local -a paths=()

  if [[ -n "${DIFF_BASE}" ]]; then
    git rev-parse --verify "${DIFF_BASE}^{commit}" >/dev/null
    mapfile -d '' -t paths < <(
      git diff --name-only --diff-filter=ACMRTUXB -z "${DIFF_BASE}" --
    )
  else
    mapfile -d '' -t paths < <(git ls-files -z)
  fi

  local path
  for path in "${paths[@]}"; do
    if [[ "${path}" =~ ${pattern} && -f "${path}" ]]; then
      result+=("${path}")
    fi
  done
}

run_cpp_lint() {
  local -a cpp_files=()
  local -a bazel_files=()
  collect_files '\.(c|cc|cpp|cu|h|hh|hpp|hxx|cxx)$' cpp_files
  collect_files '(^|/)BUILD(\.bazel)?$|\.bzl$|\.bazel$' bazel_files

  if [[ "${#cpp_files[@]}" -gt 0 ]]; then
    local clang_format_cmd="${CLANG_FORMAT_CMD:-clang-format}"
    require_command "${clang_format_cmd}"
    printf 'Checking C++ formatting (%s files)\n' "${#cpp_files[@]}"
    printf '%s\0' "${cpp_files[@]}" |
      xargs -0 -r "${clang_format_cmd}" --dry-run --Werror
  fi

  if [[ "${#bazel_files[@]}" -gt 0 ]]; then
    require_command buildifier
    printf 'Checking Bazel formatting and lint (%s files)\n' "${#bazel_files[@]}"
    printf '%s\0' "${bazel_files[@]}" |
      xargs -0 -r buildifier -mode=check -lint=warn
  fi
}

run_python_lint() {
  local -a python_files=()
  collect_files '\.py$' python_files
  if [[ "${#python_files[@]}" -eq 0 ]]; then
    return 0
  fi

  require_command black
  require_command isort
  require_command flake8
  printf 'Checking Python formatting and lint (%s files)\n' "${#python_files[@]}"
  black --check "${python_files[@]}"
  isort --check-only --profile black "${python_files[@]}"
  flake8 "${python_files[@]}"
}

run_shell_lint() {
  local -a shell_files=()
  collect_files '\.(sh|bashrc)$' shell_files
  if [[ "${#shell_files[@]}" -eq 0 ]]; then
    return 0
  fi

  require_command shellcheck
  printf 'Checking shell scripts (%s files)\n' "${#shell_files[@]}"
  shellcheck -x --shell=bash "${shell_files[@]}"
}

parse_args() {
  if [[ "$#" -eq 0 ]]; then
    CPP_LINT_FLAG=1
    PYTHON_LINT_FLAG=1
    SHELL_LINT_FLAG=1
    return
  fi

  while [[ "$#" -gt 0 ]]; do
    case "$1" in
      --cpp) CPP_LINT_FLAG=1 ;;
      --py) PYTHON_LINT_FLAG=1 ;;
      --sh) SHELL_LINT_FLAG=1 ;;
      --diff)
        if [[ "$#" -lt 2 || "$2" == -* ]]; then
          echo "--diff requires a commitish argument." >&2
          return 1
        fi
        DIFF_BASE="$2"
        shift
        ;;
      -a|--all)
        CPP_LINT_FLAG=1
        PYTHON_LINT_FLAG=1
        SHELL_LINT_FLAG=1
        ;;
      -h|--help)
        print_usage
        exit 0
        ;;
      *)
        echo "Unknown option: $1" >&2
        print_usage
        return 1
        ;;
    esac
    shift
  done

  if [[ "${CPP_LINT_FLAG}" -eq 0 &&
        "${PYTHON_LINT_FLAG}" -eq 0 &&
        "${SHELL_LINT_FLAG}" -eq 0 ]]; then
    print_usage
    echo "Select at least one lint check." >&2
    return 1
  fi
}

main() {
  parse_args "$@"

  if [[ "${CPP_LINT_FLAG}" -eq 1 ]]; then
    run_cpp_lint
  fi
  if [[ "${PYTHON_LINT_FLAG}" -eq 1 ]]; then
    run_python_lint
  fi
  if [[ "${SHELL_LINT_FLAG}" -eq 1 ]]; then
    run_shell_lint
  fi

  echo "All selected lint checks passed."
}

main "$@"
