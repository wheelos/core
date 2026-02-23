# Copyright 2018 The Apollo Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Module extension that exposes the system-installed FlatBuffers headers
as a Bazel repository named @flatbuffers.

The FlatBuffers C++ runtime is header-only, so no library linking is required.
The repository rule simply symlinks the system include directory and writes a
minimal BUILD file that declares a cc_library for the headers.
"""

def _system_flatbuffers_impl(ctx):
    include_dir = ctx.os.environ.get(
        "FLATBUFFERS_INCLUDE_DIR",
        "/usr/include",
    )
    ctx.symlink(include_dir + "/flatbuffers", "flatbuffers")
    ctx.file("BUILD", """
load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

# Header-only FlatBuffers runtime library.
cc_library(
    name = "flatbuffers",
    hdrs = glob(["flatbuffers/**/*.h"]),
    includes = ["."],
)
""")

_system_flatbuffers = repository_rule(
    implementation = _system_flatbuffers_impl,
    local = True,
    environ = ["FLATBUFFERS_INCLUDE_DIR"],
)

def _flatbuffers_extension_impl(module_ctx):
    _system_flatbuffers(name = "flatbuffers")

flatbuffers_extension = module_extension(
    implementation = _flatbuffers_extension_impl,
)
