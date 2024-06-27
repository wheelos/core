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

# Usage:
#   shfmt.sh <path/to/src/dir/or/files>

# Fail on error
set -e

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
source "${ROOT_DIR}/scripts/deps/installer_base.sh"

SHELL_FORMAT_CMD="shfmt"

# Function to find all shell script files
function find_shell_scripts() {
  find "$@" -type f \( -name "*.sh" -o -name "*.bash" -o -name "*.bashrc" \)
}

# Function to check if the file has a shell script extension
function is_shell_script() {
  local ext
  ext="$(file_ext "$1")"
  for valid_ext in "sh" "bash" "bashrc"; do
    if [[ "${ext}" == "${valid_ext}" ]]; then
      return 0
    fi
  done
  return 1
}

# Function to check if shfmt is installed
function check_shfmt() {
  if ! command -v ${SHELL_FORMAT_CMD} &> /dev/null; then
    error "shfmt not found."
    error "Please make sure shfmt is installed and check your PATH settings."
    exit 1
  fi
}

# Function to run shfmt on the provided files
function shell_format_run() {
  ${SHELL_FORMAT_CMD} -w "$@"
}

# Function to process files or directories
function run_shfmt() {
  for target in "$@"; do
    if [[ -f "${target}" ]]; then
      if is_shell_script "${target}"; then
        shell_format_run "${target}"
        info "Done formatting ${target}"
      else
        warning "Do nothing. ${target} is not a shell script."
      fi
    else
      local srcs
      srcs="$(find_shell_scripts "${target}")"
      if [[ -z "${srcs}" ]]; then
        ok "No shell scripts found under ${target}"
        continue
      fi
      shell_format_run ${srcs}
      ok "Done formatting shell scripts under ${target}"
    fi
  done
}

function main() {
  check_shfmt

  if [[ "$#" -eq 0 ]]; then
    error "Usage: $0 <path/to/dirs/or/files>"
    exit 1
  fi

  run_shfmt "$@"
}

main "$@"
