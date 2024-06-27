#!/bin/bash

# Install clang-format, shfmt
apt-get -y update && \
    apt-get -y install \
    clang-format \
    npm \
    shfmt

# Install autopep8
pip install -q autopep8

# Install prettier
npm install -g --save-dev --save-exact prettier
