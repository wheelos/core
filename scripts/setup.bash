#! /usr/bin/env bash

# Define the top directory
TOP_DIR="$(cd "$( dirname "${BASH_SOURCE[0]}" )/.." && pwd -P)"
source "${TOP_DIR}/scripts/env/common.bashrc"

# Define wheel root directory
WHEEL_ROOT_DIR="${TOP_DIR}"

# Prepend each tool path to PATH
bazel_bin_path="${WHEEL_ROOT_DIR}/bazel-bin"
declare -A tools_paths=(
    ["mainboard"]="${bazel_bin_path}/cyber/mainboard"
    ["recorder"]="${bazel_bin_path}/cyber/tools/cyber_recorder"
    ["monitor"]="${bazel_bin_path}/cyber/tools/cyber_monitor"
    ["channel"]="${bazel_bin_path}/cyber/tools/cyber_channel"
    ["node"]="${bazel_bin_path}/cyber/tools/cyber_node"
    ["service"]="${bazel_bin_path}/cyber/tools/cyber_service"
    ["launch"]="${bazel_bin_path}/cyber/tools/cyber_launch"
    ["visualizer"]="${bazel_bin_path}/tools/visualizer"
)

for tool in "${!tools_paths[@]}"; do
    pathprepend "${tools_paths[$tool]}"
done

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

export sysmo_start=0
