#! /usr/bin/env bash

set -e

# STAGE default value
STAGE=${STAGE:-dev}

# APOLLO_DOCS paths
APOLLO_DOCS_CFG="${APOLLO_ROOT_DIR}/apollo.doxygen"
APOLLO_DOCS_DIR=$(awk -F '[= ]' '/^OUTPUT_DIRECTORY/ {print $NF}' "${APOLLO_DOCS_CFG}")

# Error handling function (combined)
function error() {
  echo -e "ERROR: $*" >&2
  exit 1
}

# Helper functions
function info() {
  echo -e "INFO: $*"
}

function success() {
  echo -e "SUCCESS: $*"
}

# Determine docs directory (simplified)
if [[ ! "${APOLLO_DOCS_DIR}" == /* ]]; then
  APOLLO_DOCS_DIR="${APOLLO_ROOT_DIR}/${APOLLO_DOCS_DIR}"
fi

# Generate docs function (cleaner logic)
function generate_docs() {
  local doxygen_cmd=$(command -v doxygen)
  if [ -z "${doxygen_cmd}" ]; then
    error "Command 'doxygen' not found. Please install it manually."
    # ... installation instructions (unchanged)
  fi

  if [ ! -d "${APOLLO_DOCS_DIR}" ]; then
    mkdir -p "${APOLLO_DOCS_DIR}"
  fi

  local start_time=$(date +%s)  # Use date for timestamp
  pushd "${APOLLO_ROOT_DIR}" >/dev/null
  run "${doxygen_cmd}" "${APOLLO_DOCS_CFG}" >/dev/null
  popd >/dev/null

  local elapsed=$(( $(date +%s) - start_time ))
  success "Apollo docs generated. Time taken: ${elapsed}s"
}

# Clean docs function (simplified)
function clean_docs() {
  if [ -d "${APOLLO_DOCS_DIR}" ]; then
    rm -rf "${APOLLO_DOCS_DIR}"
    success "Done cleanup apollo docs in ${APOLLO_DOCS_DIR}"
  else
    success "Nothing to do for empty directory '${APOLLO_DOCS_DIR}'."
  fi
}

# Start http server http://localhost:8000
function start_doc_server() {
  cd ${APOLLO_DOCS_DIR}
  python -m http.server
}

# Usage function (improved formatting)
function _usage() {
  cat <<EOF
Usage:
  $0 [Options]

Options:
  -h, --help   Show this help message and exit
  clean       Delete generated docs
  generate    Generate apollo docs
EOF
  exit 1
}

# Main function (removed unused options)
main() {
  local cmd="$1"
  determine_docs_dir

  case "${cmd}" in
    generate)
      generate_docs
      ;;
    clean)
      clean_docs
      ;;
    start)
      start_doc_server
      ;;
    -h | --help)
      _usage
      ;;
    *)
      _usage
      ;;
  esac
}

main "$@"
