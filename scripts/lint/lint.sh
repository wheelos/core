#!/usr/bin/env bash

# Fail on error
set -e

TOP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
source "${TOP_DIR}/scripts/apollo.bashrc"
source "${TOP_DIR}/scripts/apollo_base.sh"

: ${STAGE:=dev}

# Initialize lint flags
PYTHON_LINT_FLAG=0
CPP_LINT_FLAG=0
SHELL_LINT_FLAG=0

# Run C++ lint
function run_cpp_lint() {
  pushd "${APOLLO_ROOT_DIR}" >/dev/null
  local cpp_dirs="cyber"
  [[ "${STAGE}" == "dev" ]] && cpp_dirs="${cpp_dirs} modules"
  for prey in $(find ${cpp_dirs} -name BUILD | xargs grep -l -E 'cc_library|cc_test|cc_binary|gpu_library' | xargs grep -L 'cpplint()'); do
    warning "unattended BUILD file found: ${prey}. Add cpplint() automatically."
    sed -i '1i\load("//tools:cpplint.bzl", "cpplint")\n' "${prey}"
    sed -i -e '$a\\ncpplint()' "${prey}"
    [[ -x "$(command -v buildifier)" ]] && buildifier -lint=fix "${prey}"
  done
  popd >/dev/null

  local targets="//cyber/..."
  bazel test --config=cpplint "${targets}"
  [[ "${STAGE}" == "dev" ]] && bazel test --config=cpplint "//modules/..."
}

# Run shell lint
function run_sh_lint() {
  local shellcheck_cmd
  shellcheck_cmd="$(command -v shellcheck)"
  if [ -z "${shellcheck_cmd}" ]; then
    warning "Command 'shellcheck' not found. Please install it via: sudo apt-get -y update && sudo apt-get -y install shellcheck"
    exit 1
  fi

  local sh_dirs="cyber scripts docker tools"
  [[ "${STAGE}" == "dev" ]] && sh_dirs="modules ${sh_dirs}"

  sh_dirs=$(printf "${APOLLO_ROOT_DIR}/%s " ${sh_dirs})
  run find ${sh_dirs} -type f \( -name "*.sh" -or -name "*.bashrc" \) -exec shellcheck -x --shell=bash {} +

  for script in ${APOLLO_ROOT_DIR}/*.sh; do
    run shellcheck -x --shell=bash "${script}"
  done
}

# Run Python lint
function run_py_lint() {
  local flake8_cmd
  flake8_cmd="$(command -v flake8)"
  if [ -z "${flake8_cmd}" ]; then
    warning "Command 'flake8' not found. Install it via: '[sudo -H] python3 -m pip install flake8'"
    exit 1
  fi

  local py_dirs="cyber docker tools"
  [[ "${STAGE}" == "dev" ]] && py_dirs="modules ${py_dirs}"

  py_dirs=$(printf "${APOLLO_ROOT_DIR}/%s " ${py_dirs})
  run find ${py_dirs} -type f -name "*.py" -exec flake8 {} \;
}

# Print usage information
function print_usage() {
  info "Usage: $0 [Options]"
  info "Options:"
  info "${TAB}--py        Lint Python files"
  info "${TAB}--sh        Lint Bash scripts"
  info "${TAB}--cpp       Lint C++ source files"
  info "${TAB}-a|--all    Lint all. Equivalent to '--py --sh --cpp'"
  info "${TAB}-h|--help   Show this message and exit"
}

# Parse command line arguments
function parse_cmdline_args() {
  if [[ "$#" -eq 0 ]]; then
    CPP_LINT_FLAG=1
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
      --sh)
        SHELL_LINT_FLAG=1
        ;;
      -a|--all)
        PYTHON_LINT_FLAG=1
        CPP_LINT_FLAG=1
        SHELL_LINT_FLAG=1
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

# Main function
function main() {
  # Restore environment
  sed -i 's/STATUS = 2/STATUS = 0/g' /apollo/tools/package/dynamic_deps.bzl
  [[ -e "${TOP_DIR}/WORKSPACE.source" ]] && rm -f "${TOP_DIR}/WORKSPACE" && cp "${TOP_DIR}/WORKSPACE.source" "${TOP_DIR}/WORKSPACE"

  # Parse command line arguments and run corresponding lints
  parse_cmdline_args "$@"
  [[ "${CPP_LINT_FLAG}" -eq 1 ]] && run_cpp_lint
  [[ "${PYTHON_LINT_FLAG}" -eq 1 ]] && run_py_lint
  [[ "${SHELL_LINT_FLAG}" -eq 1 ]] && run_sh_lint
}

main "$@"
