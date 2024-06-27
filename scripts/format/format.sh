#!/usr/bin/env bash

# Usage:
#   apollo_format.sh [options] <path/to/src/dir/or/files>

# Fail on error
set -e

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
source "${ROOT_DIR}/scripts/deps/installer_base.sh"

# Initialize format options
FORMAT_BAZEL=0
FORMAT_CPP=0
FORMAT_MARKDOWN=0
FORMAT_PYTHON=0
FORMAT_SHELL=0
FORMAT_ALL=0
HAS_OPTION=0

# Print usage information
function print_usage() {
  echo -e "\n${RED}Usage${NO_COLOR}:
  .${BOLD}$0${NO_COLOR} [OPTION] <path/to/src/dir/or/files>"
  echo -e "\n${RED}Options${NO_COLOR}:
  ${BLUE}-p|--python          ${NO_COLOR}Format Python code
  ${BLUE}-b|--bazel           ${NO_COLOR}Format Bazel code
  ${BLUE}-c|--cpp             ${NO_COLOR}Format C++ code
  ${BLUE}-s|--shell           ${NO_COLOR}Format Shell code
  ${BLUE}-m|--markdown        ${NO_COLOR}Format Markdown file
  ${BLUE}-a|--all             ${NO_COLOR}Format all
  ${BLUE}-h|--help            ${NO_COLOR}Show this message and exit"
}

# Format functions
function run_clang_format() {
  bash "${TOP_DIR}/scripts/format/clang_format.sh" "$@"
}

function run_buildifier() {
  bash "${TOP_DIR}/scripts/format/buildifier.sh" "$@"
}

function run_autopep8() {
  bash "${TOP_DIR}/scripts/format/autopep8.sh" "$@"
}

function run_shfmt() {
  bash "${TOP_DIR}/scripts/format/shfmt.sh" "$@"
}

function run_prettier() {
  bash "${TOP_DIR}/scripts/format/prettier.sh" "$@"
}

# Main format function
function run_apollo_format() {
  for arg in "$@"; do
    if [[ -f "${arg}" ]]; then
      if c_family_ext "${arg}" || proto_ext "${arg}"; then
        run_clang_format "${arg}"
      elif py_ext "${arg}"; then
        run_autopep8 "${arg}"
      elif prettier_ext "${arg}"; then
        run_prettier "${arg}"
      elif bazel_extended "${arg}"; then
        run_buildifier "${arg}"
      elif bash_ext "${arg}"; then
        run_shfmt "${arg}"
      fi
    elif [[ -d "${arg}" ]]; then
      [ "${FORMAT_BAZEL}" -eq 1 ] && run_buildifier "${arg}"
      [ "${FORMAT_CPP}" -eq 1 ] && run_clang_format "${arg}"
      [ "${FORMAT_PYTHON}" -eq 1 ] && run_autopep8 "${arg}"
      [ "${FORMAT_SHELL}" -eq 1 ] && run_shfmt "${arg}"
      [ "${FORMAT_MARKDOWN}" -eq 1 ] && run_prettier "${arg}"
    else
      warning "Ignored ${arg} as not a regular file/directory"
    fi
  done
}

# Main function
function main() {
  if [ "$#" -eq 0 ]; then
    print_usage
    exit 1
  fi

  while [ $# -gt 0 ]; do
    local opt="$1"
    case "${opt}" in
      -p | --python) FORMAT_PYTHON=1; HAS_OPTION=1 ;;
      -c | --cpp) FORMAT_CPP=1; HAS_OPTION=1 ;;
      -b | --bazel) FORMAT_BAZEL=1; HAS_OPTION=1 ;;
      -s | --shell) FORMAT_SHELL=1; HAS_OPTION=1 ;;
      -m | --markdown) FORMAT_MARKDOWN=1; HAS_OPTION=1 ;;
      -a | --all) FORMAT_ALL=1 ;;
      -h | --help) print_usage; exit 1 ;;
      *)
        if [[ "${opt}" = -* ]]; then
          print_usage
          exit 1
        else
          [ "${HAS_OPTION}" -eq 0 ] && FORMAT_ALL=1
          break
        fi
        ;;
    esac
    shift
  done

  if [ "${FORMAT_ALL}" -eq 1 ]; then
    FORMAT_BAZEL=1
    FORMAT_CPP=1
    FORMAT_MARKDOWN=1
    FORMAT_SHELL=1
    FORMAT_PYTHON=1
  fi

  run_apollo_format "$@"
}

main "$@"
