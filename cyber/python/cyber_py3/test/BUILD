load("@rules_python//python:defs.bzl", "py_test")

package(default_visibility = ["//:internal"])

py_test(
    name = "record_test",
    timeout = "short",
    srcs = ["record_test.py"],
    deps = [
        "//cyber/proto:record_py_pb2",
        "//cyber/python/cyber_py3:record",
    ],
)

py_test(
    name = "init_test",
    timeout = "short",
    srcs = ["init_test.py"],
    deps = [
        "//cyber/python/cyber_py3:cyber",
    ],
)

py_test(
    name = "cyber_test",
    timeout = "short",
    srcs = ["cyber_test.py"],
    deps = [
        "//cyber/proto:unit_test_py_pb2",
        "//cyber/python/cyber_py3:cyber",
    ],
)

py_test(
    name = "parameter_test",
    timeout = "short",
    srcs = ["parameter_test.py"],
    deps = [
        "//cyber/python/cyber_py3:cyber",
        "//cyber/python/cyber_py3:parameter",
    ],
)

py_test(
    name = "cyber_time_test",
    timeout = "short",
    srcs = ["cyber_time_test.py"],
    deps = [
        "//cyber/python/cyber_py3:cyber",
        "//cyber/python/cyber_py3:cyber_time",
    ],
)

py_test(
    name = "cyber_timer_test",
    timeout = "short",
    srcs = ["cyber_timer_test.py"],
    deps = [
        "//cyber/python/cyber_py3:cyber",
        "//cyber/python/cyber_py3:cyber_timer",
    ],
)

py_test(
    name = "shutdown_exit_test",
    timeout = "short",
    srcs = ["shutdown_exit_test.py"],
    deps = [
        "//cyber/proto:unit_test_py_pb2",
        "//cyber/python/cyber_py3:cyber",
        "//cyber/python/cyber_py3:cyber_timer",
        "//cyber/python/cyber_py3:parameter",
    ],
)
