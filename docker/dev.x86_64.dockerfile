FROM docker.m.daocloud.io/library/ubuntu:22.04

# Avoid interactive prompts during apt-get
ENV DEBIAN_FRONTEND=noninteractive

# Update apt source to ali mirrors for faster downloads inside China
RUN sed -i 's/archive.ubuntu.com/mirrors.aliyun.com/g' /etc/apt/sources.list && \
    sed -i 's/security.ubuntu.com/mirrors.aliyun.com/g' /etc/apt/sources.list

# Install basic dependencies (wget, curl, sudo, build-essential, g++, unzip, zip, python3, zlib1g-dev, git)
RUN apt-get update && apt-get install -y \
    wget \
    curl \
    sudo \
    build-essential \
    g++ \
    unzip \
    zip \
    python3 \
    python3-venv \
    patchelf \
    dpkg-dev \
    zlib1g-dev \
    git \
    && rm -rf /var/lib/apt/lists/*

# Keep the default runtime identity stable across clean containers. Override
# these arguments when a bind-mounted checkout uses a different host UID/GID.
ARG WHEELOS_UID=1000
ARG WHEELOS_GID=1000
RUN groupadd --gid "${WHEELOS_GID}" wheelos && \
    useradd --uid "${WHEELOS_UID}" --gid wheelos --create-home --shell /bin/bash wheelos

# Add runtime.bash sourcing to the global bashrc so that any interactive bash shell
# (root or host user) automatically gets the environment.
RUN echo "[ -f /workspace/scripts/env/runtime.bash ] && source /workspace/scripts/env/runtime.bash" >> /etc/bash.bashrc

WORKDIR /tmp/build
ARG BAZEL_VERSION=7.6.2
RUN curl -fsSL \
    "https://github.com/bazelbuild/bazel/releases/download/${BAZEL_VERSION}/bazel-${BAZEL_VERSION}-installer-linux-x86_64.sh" \
    -o bazel-installer.sh && \
    bash bazel-installer.sh --prefix=/usr/local && \
    bazel --version

# Clean up build files
WORKDIR /workspace
RUN rm -rf /tmp/build

ENV HOME=/home/wheelos
ENV USER=wheelos

# Set up entrypoint script
COPY docker/entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

USER wheelos
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
CMD ["sleep", "infinity"]
