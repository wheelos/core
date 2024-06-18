#!/bin/bash


# Exit immediately on error
set -e

CURR_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
source "${CURR_DIR}/installer_base.sh"

# Determine target architecture
TARGET_ARCH=$(uname -m)

# Get Bazel version from file
BAZEL_VERSION=$(<"${CURR_DIR}/../../.bazelversion")
BUILDTOOLS_VERSION="7.1.2"
SYSROOT_BIN_DIR="${SYSROOT_DIR}/bin"

install_bazel() {
  local pkg_name=$1
  local checksum=$2
  local download_link="https://github.com/bazelbuild/bazel/releases/download/${BAZEL_VERSION}/${pkg_name}"

  download_if_not_cached "${pkg_name}" "${checksum}" "${download_link}"

  if [[ "${pkg_name}" == *.deb ]]; then
    apt_get_update_and_install g++ unzip zip
    dpkg -i "${pkg_name}"
  else
    cp -f "${pkg_name}" "${SYSROOT_BIN_DIR}/bazel"
    chmod a+x "${SYSROOT_BIN_DIR}/bazel"
  fi

  rm -f "${pkg_name}"
}

install_buildifier() {
  local arch=$1
  local checksum=$2

  local pkg_name="buildifier-linux-${arch}"
  local download_link="https://github.com/bazelbuild/buildtools/releases/download/v${BUILDTOOLS_VERSION}/${pkg_name}"

  download_if_not_cached "${pkg_name}" "${checksum}" "${download_link}"

  cp -f "${pkg_name}" "${SYSROOT_BIN_DIR}/buildifier"
  chmod a+x "${SYSROOT_BIN_DIR}/buildifier"
  rm -f "${pkg_name}"
}

cleanup() {
  apt-get clean
  rm -rf /var/lib/apt/lists/*
}

trap cleanup EXIT

echo "Installing Bazel ${BAZEL_VERSION}..."

case "$TARGET_ARCH" in
  x86_64)
    BAZEL_PKG="bazel_${BAZEL_VERSION}-linux-x86_64.deb"
    BAZEL_CHECKSUM="70c0166145f649d53f1306f19ff8763ff3ac5777f0e9eb48e30f4abb0dfb4caf"
    install_bazel "${BAZEL_PKG}" "${BAZEL_CHECKSUM}"

    BUILDTOOLS_CHECKSUM="5d47f5f452bace65686448180ff63b4a6aaa0fb0ce0fe69976888fa4d8606940"
    install_buildifier "amd64" "${BUILDTOOLS_CHECKSUM}"
    ;;

  aarch64)
    BAZEL_PKG="bazel-${BAZEL_VERSION}-linux-arm64"
    BAZEL_CHECKSUM="d621ba80c471531e208773f005913169ce226d885eebd9c81ca156bd25761149"
    install_bazel "${BAZEL_PKG}" "${BAZEL_CHECKSUM}"

    cp /opt/apollo/rcfiles/bazel_completion.bash /etc/bash_completion.d/bazel

    BUILDTOOLS_CHECKSUM="c22a44eee37b8927167ee6ee67573303f4e31171e7ec3a8ea021a6a660040437"
    install_buildifier "arm64" "${BUILDTOOLS_CHECKSUM}"
    ;;

  *)
    echo "Error: Target architecture ${TARGET_ARCH} not supported yet"
    exit 1
    ;;
esac

info "Done installing Bazel ${BAZEL_VERSION} with Buildifier ${BUILDTOOLS_VERSION}"
