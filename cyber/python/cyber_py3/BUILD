load("@rules_python//python:defs.bzl", "py_library")

package(default_visibility = ["//visibility:public"])

filegroup(
    name = "runtime_files",
    srcs = glob([
        "*.py",
    ]),
)

py_library(
    name = "internal",
    srcs = ["_internal.py"],
    data = [
        "//cyber:runtime_resources",
        "//cyber/python/internal:_cyber_wrapper",
    ],
)

py_library(
    name = "cyber_time",
    srcs = ["cyber_time.py"],
    deps = [":internal"],
)

py_library(
    name = "cyber",
    srcs = ["cyber.py"],
    deps = [
        ":internal",
        "@com_google_protobuf//:protobuf_python",
    ],
)

py_library(
    name = "cyber_timer",
    srcs = ["cyber_timer.py"],
    deps = [
        ":cyber",
        ":internal",
    ],
)

py_library(
    name = "parameter",
    srcs = ["parameter.py"],
    deps = [
        ":internal",
    ],
)

py_library(
    name = "record",
    srcs = ["record.py"],
    deps = [
        ":cyber",
        ":internal",
        "@com_google_protobuf//:protobuf_python",
    ],
)
