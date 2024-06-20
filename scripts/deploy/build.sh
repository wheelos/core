#!/bin/bash

# Exit immediately if a command exits with a non-zero status
set -e


# Install the compilation tools
scripts/deps/install_bazel.sh

# Install dependent libraries
scripts/deps/install_fast-rtps.sh
scripts/deps/install_uuid.sh

echo "All dependencies have been successfully installed."
