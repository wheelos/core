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

# Fail on first error.
set -e

CURR_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
. ${CURR_DIR}/installer_base.sh

TARGET_ARCH=$(uname -m)

BAZEL_VERSION=$(<"${CURR_DIR}/../../.bazelversion")
echo "Start installing bazel ${BAZEL_VERSION}"

if [ "$TARGET_ARCH" == "x86_64" ]; then
  # https://bazel.build/install/ubuntu#binary-installer
  PKG_NAME="bazel_${BAZEL_VERSION}-linux-x86_64.deb"
  DOWNLOAD_LINK="https://github.com/bazelbuild/bazel/releases/download/${BAZEL_VERSION}/${PKG_NAME}"
  SHA256SUM="70c0166145f649d53f1306f19ff8763ff3ac5777f0e9eb48e30f4abb0dfb4caf"
  download_if_not_cached $PKG_NAME $SHA256SUM $DOWNLOAD_LINK

  apt_get_update_and_install \
    g++ unzip zip

  # https://docs.bazel.build/versions/master/install-ubuntu.html#step-3-install-a-jdk-optional
  # openjdk-11-jdk

  dpkg -i "${PKG_NAME}"

  # Cleanup right after installation
  rm -rf "${PKG_NAME}"

  ## buildifier ##
  BUILDTOOLS_VERSION="7.1.2"
  PKG_NAME="buildifier-linux-amd64"
  CHECKSUM="5d47f5f452bace65686448180ff63b4a6aaa0fb0ce0fe69976888fa4d8606940"
  DOWNLOAD_LINK="https://github.com/bazelbuild/buildtools/releases/download/v${BUILDTOOLS_VERSION}/${PKG_NAME}"
  download_if_not_cached "${PKG_NAME}" "${CHECKSUM}" "${DOWNLOAD_LINK}"

  cp -f ${PKG_NAME} "${SYSROOT_DIR}/bin/buildifier"
  chmod a+x "${SYSROOT_DIR}/bin/buildifier"
  rm -rf ${PKG_NAME}

  info "Done installing bazel ${BAZEL_VERSION} with buildifier ${BUILDTOOLS_VERSION}"

elif [ "$TARGET_ARCH" == "aarch64" ]; then
  ARM64_BINARY="bazel-${BAZEL_VERSION}-linux-arm64"
  CHECKSUM="d621ba80c471531e208773f005913169ce226d885eebd9c81ca156bd25761149"
  DOWNLOAD_LINK="https://github.com/bazelbuild/bazel/releases/download/${BAZEL_VERSION}/${ARM64_BINARY}"
  # https://github.com/bazelbuild/bazel/releases/download/7.2.0/bazel-7.2.0-linux-arm64
  download_if_not_cached "${ARM64_BINARY}" "${CHECKSUM}" "${DOWNLOAD_LINK}"
  cp -f ${ARM64_BINARY} "${SYSROOT_DIR}/bin/bazel"
  chmod a+x "${SYSROOT_DIR}/bin/bazel"
  rm -rf "${ARM64_BINARY}"

  cp /opt/apollo/rcfiles/bazel_completion.bash /etc/bash_completion.d/bazel
  
  ## buildifier ##
  BUILDTOOLS_VERSION="7.1.2"
  PKG_NAME="buildifier-linux-arm64"
  CHECKSUM="c22a44eee37b8927167ee6ee67573303f4e31171e7ec3a8ea021a6a660040437"
  DOWNLOAD_LINK="https://github.com/bazelbuild/buildtools/releases/download/v${BUILDTOOLS_VERSION}/${PKG_NAME}"
  download_if_not_cached "${PKG_NAME}" "${CHECKSUM}" "${DOWNLOAD_LINK}"

  cp -f ${PKG_NAME} "${SYSROOT_DIR}/bin/buildifier"
  chmod a+x "${SYSROOT_DIR}/bin/buildifier"
  rm -rf ${PKG_NAME}

  info "Done installing bazel ${BAZEL_VERSION} with buildifier ${BUILDTOOLS_VERSION}"
else
  error "Target arch ${TARGET_ARCH} not supported yet"
  exit 1
fi

# Clean up cache to reduce layer size.
apt-get clean && \
    rm -rf /var/lib/apt/lists/*

