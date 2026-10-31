load("@rules_python//python:defs.bzl", "py_binary")

package(
    default_visibility = ["//:internal"],
)

exports_files(
    ["cyber_launch.py"],
    visibility = ["//visibility:public"],
)

# FIXME(all): python module deps
py_binary(
    name = "cyber_launch",
    srcs = ["cyber_launch.py"],
    data = [
        "//cyber:runtime_resources",
        "//cyber/mainboard",
    ],
    visibility = ["//visibility:public"],
)
