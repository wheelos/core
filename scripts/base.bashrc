#!/usr/bin/env bash

###############################################################################
# Copyright 2020 The Apollo Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
###############################################################################

# Set top directory
TOP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"

# Set verbose mode
: ${VERBOSE:=yes}

# Color definitions
BOLD='\033[1m'
RED='\033[0;31m'
BLUE='\033[1;34m'
GREEN='\033[0;32m'
WHITE='\033[0;37m'
YELLOW='\033[0;33m'
NO_COLOR='\033[0m'

# Utility functions for colored output
function info() {
  echo -e "[${WHITE}${BOLD}INFO${NO_COLOR}] $*" >&2
}

function error() {
  echo -e "[${RED}ERROR${NO_COLOR}] $*" >&2
}

function warning() {
  echo -e "[${YELLOW}WARNING${NO_COLOR}] $*" >&2
}

function ok() {
  echo -e "[${GREEN}${BOLD} OK ${NO_COLOR}] $*" >&2
}

function print_delim() {
  echo "=============================================="
}

# Utility functions for time calculations
function get_now() {
  date +%s
}

function time_elapsed_s() {
  local start="${1:-$(get_now)}"
  local end
  end=$(get_now)
  echo "$((end - start))"
}

function success() {
  print_delim
  ok "$1"
  print_delim
}

function fail() {
  print_delim
  error "$1"
  print_delim
  exit 1
}

# Check and set GPU usage
function determine_gpu_use_target() {
  local arch
  arch=$(uname -m)
  local gpu_platform="UNKNOWN"
  local use_gpu=0
  local nv=0
  local amd=0
  local need_cuda=0
  local need_rocm=0

  if [[ "${arch}" == "aarch64" ]]; then
    if lsmod | grep -q nvgpu; then
      if ldconfig -p | grep -q cudart; then
        use_gpu=1
        need_cuda=1
        gpu_platform="NVIDIA"
      fi
    fi
  else
    if command -v nvidia-smi &>/dev/null && [[ -n "$(nvidia-smi)" ]]; then
      nv=0
      use_gpu=1
      need_cuda=1
      gpu_platform="NVIDIA"
      info "NVIDIA GPU device found."
    else
      nv=1
      info "No NVIDIA GPU device found."
    fi

    if command -v rocm-smi &>/dev/null && [[ -n "$(rocm-smi)" ]]; then
      amd=0
      use_gpu=1
      need_rocm=1
      gpu_platform="AMD"
      info "AMD GPU device found."
    else
      amd=1
      info "No AMD GPU device found."
    fi

    if ((nv == 0 && amd == 0)); then
      info "NVIDIA GPU device is chosen for the build."
    fi
  fi

  export TF_NEED_CUDA="${need_cuda}"
  export TF_NEED_ROCM="${need_rocm}"
  export USE_GPU_TARGET="${use_gpu}"
  export GPU_PLATFORM="${gpu_platform}"
}

# Utility functions for file extensions and searches
function file_ext() {
  echo "${1##*.}"
}

function c_family_ext() {
  local ext
  ext=$(file_ext "$1")
  [[ "$ext" =~ ^(h|hh|hxx|hpp|cxx|cc|cpp|cu)$ ]]
}

function find_c_cpp_srcs() {
  find "$@" \( -name "*.h" -o -name "*.c" -o -name "*.hpp" -o -name "*.cpp" -o -name "*.hh" -o -name "*.cc" -o -name "*.hxx" -o -name "*.cxx" -o -name "*.cu" \)
}

function proto_ext() {
  [[ "$(file_ext "$1")" == "proto" ]]
}

function find_proto_srcs() {
  find "$@" -name "*.proto"
}

function py_ext() {
  [[ "$(file_ext "$1")" == "py" ]]
}

function find_py_srcs() {
  find "$@" -name "*.py"
}

function bash_ext() {
  local ext
  ext=$(file_ext "$1")
  [[ "$ext" =~ ^(sh|bash|bashrc)$ ]]
}

function prettier_ext() {
  local ext
  ext=$(file_ext "$1")
  [[ "$ext" =~ ^(md|json|yml)$ ]]
}

function find_prettier_srcs() {
  find "$@" \( -name "*.md" -o -name "*.json" -o -name "*.yml" \)
}

# Function to execute command and handle errors
function run() {
  if [ "${VERBOSE}" = yes ]; then
    echo "${@}"
    "${@}" || exit $?
  else
    local errfile="${APOLLO_ROOT_DIR}/.errors.log"
    echo "${@}" > "${errfile}"
    if ! "${@}" >> "${errfile}" 2>&1; then
      local exitcode=$?
      cat "${errfile}" >&2
      exit $exitcode
    fi
  fi
}

# Git utilities
function git_sha1() {
  if command -v git &>/dev/null && [ -d "${APOLLO_ROOT_DIR}/.git" ]; then
    git rev-parse --short HEAD 2>/dev/null || true
  fi
}

function git_date() {
  if command -v git &>/dev/null && [ -d "${APOLLO_ROOT_DIR}/.git" ]; then
    git log -1 --pretty=%ai | cut -d " " -f 1 || true
  fi
}

function git_branch() {
  if command -v git &>/dev/null && [ -d "${APOLLO_ROOT_DIR}/.git" ]; then
    git rev-parse --abbrev-ref HEAD
  else
    echo "@non-git"
  fi
}

# Read one character from stdin
function read_one_char_from_stdin() {
  local answer
  read -r -n1 answer
  echo "${answer}" | tr '[:upper:]' '[:lower:]'
}

# Check for missing argument for option
function optarg_check_for_opt() {
  local opt="$1"
  local optarg="$2"
  if [[ -z "${optarg}" || "${optarg}" =~ ^-.* ]]; then
    error "Missing parameter for ${opt}. Exiting..."
    exit 3
  fi
}
