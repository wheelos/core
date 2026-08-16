#! /usr/bin/env bash
export WHEEL_ROOT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )/../.." && pwd )"
source "${WHEEL_ROOT_DIR}/scripts/env/common.bashrc"

if [ ! -d "${WHEEL_ROOT_DIR}/data/log" ]; then
    mkdir -p "${WHEEL_ROOT_DIR}/data/log"
fi

# Resolve each tool from its stable local Bazel label. Aliases preserve the
# actual Bazel output location, so do not assume a fixed bazel-bin layout.
declare -A tools_targets=(
    ["mainboard"]="mainboard"
    ["cyber_launch"]="cyber_launch"
    ["cyber_recorder"]="cyber_recorder"
    ["cyber_monitor"]="cyber_monitor"
    ["cyber_channel"]="cyber_channel"
    ["cyber_node"]="cyber_node"
    ["cyber_service"]="cyber_service"
)

for tool in "${!tools_targets[@]}"; do
    tool_name="${tools_targets[$tool]}"
    tool_path="$(cd "${WHEEL_ROOT_DIR}" &&
        bazel cquery "//tools:${tool_name}" --output=files 2>/dev/null |
        awk -v name="${tool_name}" '$0 ~ "/" name "$" { print; exit }')"
    if [[ -n "${tool_path}" ]]; then
        pathprepend "${WHEEL_ROOT_DIR}/${tool_path%/*}"
    fi
done

bazel_bin_path="${WHEEL_ROOT_DIR}/bazel-bin"

export CYBER_PATH="${WHEEL_ROOT_DIR}/cyber"
source ${CYBER_PATH}/tools/cyber_tools_auto_complete.bash

# Prepend the Python internal path
pathprepend ${bazel_bin_path}/cyber/python/internal PYTHONPATH


export CYBER_DOMAIN_ID=80
export CYBER_IP=127.0.0.1

export GLOG_log_dir="${WHEEL_ROOT_DIR}/data/log"
export GLOG_alsologtostderr=0
export GLOG_colorlogtostderr=1
export GLOG_minloglevel=0

# for DEBUG log
#export GLOG_v=4


export TERM="${TERM:-xterm-256color}"
export TERMINFO="${TERMINFO:-/lib/terminfo/}"

export sysmo_start=0
