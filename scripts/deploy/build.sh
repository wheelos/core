#!/bin/bash

# Exit immediately if a command exits with a non-zero status
set -e


# Install the compilation tools
scripts/deps/install_bazel.sh

echo "All dependencies have been successfully installed."
