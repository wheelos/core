#!/usr/bin/env bash

# Usage:
#   apollo_format.sh [options] <path/to/src/dir/or/files>

# Fail on error
set -e

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
source "${ROOT_DIR}/scripts/deps/installer_base.sh"

# Initialize format options
FORMAT_CPP=0
FORMAT_PYTHON=0
FORMAT_ALL=0
HAS_OPTION=0

# Print usage information
function print_usage() {
	echo -e "\n${RED}Usage${NO_COLOR}:
  .${BOLD}$0${NO_COLOR} [OPTION] <path/to/src/dir/or/files>"
	echo -e "\n${RED}Options${NO_COLOR}:
  ${BLUE}-p|--python          ${NO_COLOR}Format Python code
  ${BLUE}-c|--cpp             ${NO_COLOR}Format C++/proto code
  ${BLUE}-a|--all             ${NO_COLOR}Format all supported C++ and Python files
  ${BLUE}-h|--help            ${NO_COLOR}Show this message and exit"
}

# Format functions
function run_clang_format() {
	bash "${ROOT_DIR}/scripts/format/clang_format.sh" "$@"
}

function run_autopep8() {
	bash "${ROOT_DIR}/scripts/format/autopep8.sh" "$@"
}

# Main format function
function run_apollo_format() {
	for arg in "$@"; do
		if [[ -f "${arg}" ]]; then
			if c_family_ext "${arg}" || proto_ext "${arg}"; then
				[ "${FORMAT_CPP}" -eq 1 ] && run_clang_format "${arg}"
			elif py_ext "${arg}"; then
				[ "${FORMAT_PYTHON}" -eq 1 ] && run_autopep8 "${arg}"
			fi
		elif [[ -d "${arg}" ]]; then
			[ "${FORMAT_CPP}" -eq 1 ] && run_clang_format "${arg}"
			[ "${FORMAT_PYTHON}" -eq 1 ] && run_autopep8 "${arg}"
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
		-p | --python)
			FORMAT_PYTHON=1
			HAS_OPTION=1
			;;
		-c | --cpp)
			FORMAT_CPP=1
			HAS_OPTION=1
			;;
		-a | --all)
			FORMAT_ALL=1
			FORMAT_CPP=1
			FORMAT_PYTHON=1
			;;
		-h | --help)
			print_usage
			exit 0
			;;
		*)
			if [[ "${opt}" = -* ]]; then
				print_usage
				exit 1
			else
				[ "${HAS_OPTION}" -eq 0 ] && FORMAT_ALL=1
				FORMAT_CPP=1
				FORMAT_PYTHON=1
				break
			fi
			;;
		esac
		shift
	done

	if [ "${FORMAT_ALL}" -eq 1 ]; then
		FORMAT_CPP=1
		FORMAT_PYTHON=1
	fi

	run_apollo_format "$@"
}

main "$@"
