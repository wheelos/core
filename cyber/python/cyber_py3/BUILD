load("@rules_python//python:defs.bzl", "py_library")

package(default_visibility = ["//:internal"])

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
    visibility = ["//visibility:public"],
    deps = [":internal"],
)

py_library(
    name = "cyber",
    srcs = ["cyber.py"],
    visibility = ["//visibility:public"],
    deps = [
        ":internal",
        "@com_google_protobuf//:protobuf_python",
    ],
)

py_library(
    name = "cyber_timer",
    srcs = ["cyber_timer.py"],
    visibility = ["//visibility:public"],
    deps = [
        ":cyber",
        ":internal",
    ],
)

py_library(
    name = "parameter",
    srcs = ["parameter.py"],
    visibility = ["//visibility:public"],
    deps = [
        ":internal",
    ],
)

py_library(
    name = "record",
    srcs = ["record.py"],
    visibility = ["//visibility:public"],
    deps = [
        ":cyber",
        ":internal",
        "@com_google_protobuf//:protobuf_python",
    ],
)
