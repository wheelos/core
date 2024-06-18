#! /usr/bin/env bash
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

# Ref: http://www.linuxfromscratch.org/blfs/view/svn/postlfs/profile.html
# Written for Beyond Linux From Scratch
# by James Robertson <jameswrobertson@earthlink.net>
# modifications by Dagmar d'Surreal <rivyqntzne@pbzpnfg.arg>

# Functions to help us manage paths.  Second argument is the name of the
# path variable to be modified (default: PATH)
pathremove() {
    local IFS=':'
    local NEWPATH
    local DIR
    local PATHVARIABLE=${2:-PATH}
    for DIR in ${!PATHVARIABLE}; do
        if [ "$DIR" != "$1" ]; then
            NEWPATH=${NEWPATH:+$NEWPATH:}$DIR
        fi
    done
    export $PATHVARIABLE="$NEWPATH"
}

pathprepend() {
    pathremove $1 $2
    local PATHVARIABLE=${2:-PATH}
    export $PATHVARIABLE="$1${!PATHVARIABLE:+:${!PATHVARIABLE}}"
}

pathappend() {
    pathremove $1 $2
    local PATHVARIABLE=${2:-PATH}
    export $PATHVARIABLE="${!PATHVARIABLE:+${!PATHVARIABLE}:}$1"
}

add_to_bashrc() {
    local setup_script_path="$1"
    local bashrc_file="$HOME/.bashrc"

    # Check if the setup script path is provided
    if [[ -z "$setup_script_path" ]]; then
        echo "Usage: add_to_bashrc /path/to/scripts/setup.bash"
        return 1
    fi

    # Check if the setup script is already sourced in .bashrc
    if grep -Fxq "source $setup_script_path" "$bashrc_file"; then
        echo "The setup script is already sourced in $bashrc_file"
    else
        # Add the source command to .bashrc
        echo "source $setup_script_path" >> "$bashrc_file"
        echo "Added source command to $bashrc_file"
    fi

    # Source the .bashrc file to apply changes immediately
    source "$bashrc_file"
    echo "Sourced $bashrc_file to apply changes"
}


export -f pathremove pathprepend pathappend add_to_bashrc
