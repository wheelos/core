FROM ubuntu:22.04 AS runtime-base

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates python3 python3-pip python3-venv unzip \
    libstdc++6 zlib1g libtinfo6 && rm -rf /var/lib/apt/lists/*

ARG TEST_UID=1000
ARG TEST_GID=1000
RUN groupadd --gid "${TEST_GID}" tester && \
    useradd --uid "${TEST_UID}" --gid "${TEST_GID}" --create-home tester
ENV HOME=/home/tester
USER tester
WORKDIR /tmp

FROM runtime-base AS runtime
USER root
RUN apt-get update && apt-get install -y --no-install-recommends \
    libatomic1 && rm -rf /var/lib/apt/lists/*
USER tester

FROM runtime-base AS build
USER root
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential curl git zip zlib1g-dev patchelf valgrind \
    && rm -rf /var/lib/apt/lists/*
ARG BAZEL_VERSION=8.8.1
ARG TARGETARCH
RUN case "${TARGETARCH}" in arm64) arch=arm64 ;; *) arch=x86_64 ;; esac && \
    curl -fsSL --retry 3 --connect-timeout 20 --max-time 180 \
    "https://releases.bazel.build/${BAZEL_VERSION}/release/bazel-${BAZEL_VERSION}-linux-${arch}" \
    -o /usr/local/bin/bazel && chmod +x /usr/local/bin/bazel
RUN PYTHONUSERBASE=/opt/python-userbase python3 -m pip install --user --no-cache-dir protobuf==7.36.2
USER tester
WORKDIR /workspace
