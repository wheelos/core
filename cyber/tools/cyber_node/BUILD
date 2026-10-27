load("@rules_python//python:defs.bzl", "py_binary")

package(
    default_visibility = ["//:internal"],
)

py_binary(
    name = "cyber_node",
    srcs = ["cyber_node.py"],
    visibility = ["//visibility:public"],
    deps = [
        "//cyber/proto:role_attributes_py_pb2",
        "//cyber/python/cyber_py3:cyber",
    ],
)
