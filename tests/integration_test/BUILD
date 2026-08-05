load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")
load("@rules_python//python:defs.bzl", "py_test")

cc_library(
    name = "examples_test_utils",
    hdrs = ["examples_test_utils.h"],
    deps = ["//cyber:cyber_core"],
)

cc_test(
    name = "examples_integration_test",
    size = "small",
    srcs = ["examples_integration_test.cc"],
    data = ["//cyber:cyber_conf"],
    deps = [
        "//cyber:cyber_core",
        "//cyber/examples/proto:examples_cc_proto",
        ":examples_test_utils",
        "@com_google_googletest//:gtest",
    ],
)

cc_test(
    name = "examples_stress_test",
    size = "small",
    srcs = ["examples_stress_test.cc"],
    data = ["//cyber:cyber_conf"],
    deps = [
        "//cyber:cyber_core",
        "//cyber/examples/proto:examples_cc_proto",
        ":examples_test_utils",
        "@com_google_googletest//:gtest",
    ],
)

py_test(
    name = "cyber_tools_discovery_test",
    size = "medium",
    timeout = "moderate",
    srcs = ["cyber_tools_discovery_test.py"],
    data = [
        "//cyber:cyber_conf",
        "//cyber/tools/cyber_channel:cyber_channel",
        "//cyber/tools/cyber_launch:cyber_launch",
        "//cyber/tools/cyber_monitor:cyber_monitor",
        "//cyber/tools/cyber_node:cyber_node",
        "//cyber/tools/cyber_recorder:cyber_recorder",
        "//cyber/tools/cyber_service:cyber_service",
    ],
    deps = [
        "//cyber/proto:unit_test_py_pb2",
        "//cyber/python/cyber_py3:cyber",
    ],
)

test_suite(
    name = "examples_regression_tests",
    tests = [
        ":cyber_tools_discovery_test",
        ":examples_integration_test",
        ":examples_stress_test",
        "//cyber/examples/record_play:record_play_test",
    ],
)
