#!/bin/bash

# Exit immediately if a command exits with a non-zero status
set -e


LOCAL_CACHE="/tmp/cache/"

bazel build //cyber/... --distdir="$LOCAL_CACHE"
