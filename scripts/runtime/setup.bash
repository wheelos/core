#!/usr/bin/env bash

WHEELOS_CORE_HOME="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"

export WHEELOS_CORE_HOME
export CYBER_PATH="${CYBER_PATH:-${WHEELOS_CORE_HOME}/resources/cyber}"
export PATH="${WHEELOS_CORE_HOME}/bin${PATH:+:${PATH}}"
export LD_LIBRARY_PATH="${WHEELOS_CORE_HOME}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
while IFS= read -r library_dir; do
  case ":${LD_LIBRARY_PATH}:" in
    *":${library_dir}:"*) ;;
    *) LD_LIBRARY_PATH="${LD_LIBRARY_PATH}:${library_dir}" ;;
  esac
done < <(find "${WHEELOS_CORE_HOME}/bin" -type f -name '*.so*' -printf '%h\n' 2>/dev/null | sort -u)
export LD_LIBRARY_PATH
if [[ -z "${GLOG_log_dir:-}" ]]; then
  export GLOG_log_dir="${XDG_STATE_HOME:-${HOME}/.local/state}/wheelos_core/log"
  mkdir -p "${GLOG_log_dir}"
fi
export GLOG_alsologtostderr="${GLOG_alsologtostderr:-0}"
export GLOG_colorlogtostderr="${GLOG_colorlogtostderr:-1}"
export GLOG_minloglevel="${GLOG_minloglevel:-0}"
export CYBER_DOMAIN_ID="${CYBER_DOMAIN_ID:-80}"
export CYBER_IP="${CYBER_IP:-127.0.0.1}"
