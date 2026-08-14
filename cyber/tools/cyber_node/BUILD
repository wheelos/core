load("@rules_python//python:defs.bzl", "py_binary")

package(
    default_visibility = ["//visibility:public"],
)

py_binary(
    name = "cyber_node",
    srcs = ["cyber_node.py"],
    env = {
        "PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION": "python",
    },
    deps = [
        "//cyber/proto:role_attributes_py_pb2",
        "//cyber/python/cyber_py3:cyber",
    ],
)
