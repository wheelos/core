workspace(name = "core")

load("//:bazel/core_deps.bzl", "core_deps")

core_deps()

load("//bazel/third_party/py:python_configure.bzl", "python_configure")

python_configure(name = "local_config_python")
