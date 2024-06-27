#!/usr/bin/env bash

###############################################################################
# Copyright 2020 The Apollo Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
###############################################################################

# Fail on error
set -e

# Define top directory and source apollo.bashrc
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
source "${ROOT_DIR}/scripts/deps/installer_base.sh"

NPM_DOCS="https://docs.npmjs.com/downloading-and-installing-node-js-and-npm"

# Function to check if npm and prettier are installed
function check_if_tools_installed() {
  if ! command -v npm &> /dev/null; then
    error "Please install npm first. Refer to ${NPM_DOCS} for help."
    exit 1
  fi

  if ! npm list -g | grep -q prettier; then
    error "'prettier' not found. Please install it manually by running:"
    error "  sudo npm install -g --save-dev --save-exact prettier"
    exit 1
  fi
}

# Function to run prettier
function prettier_run() {
  npx prettier --write "$@"
}

# Function to format files or directories with prettier
function format_files_with_prettier() {
  for path in "$@"; do
    if [[ -d "${path}" ]]; then
      local srcs
      srcs="$(find_prettier_srcs "${path}")"
      if [[ -n "${srcs}" ]]; then
        prettier_run ${srcs}
      fi
      ok "Done formatting markdown/json/yaml files under ${path}."
    elif [[ -f "${path}" ]]; then
      if prettier_ext "${path}"; then
        prettier_run "${path}"
      else
        warning "Only regular markdown/json/yaml files will be formatted. Ignored ${path}"
      fi
    else
      warning "Special/Symlink file won't be formatted. Ignored ${path}"
    fi
  done
}

# Main function
function main() {
  if [[ "$#" -eq 0 ]]; then
    warning "Usage: $0 <path/to/markdown/dir/or/file>"
    exit 1
  fi

  check_if_tools_installed
  format_files_with_prettier "$@"
}

main "$@"
