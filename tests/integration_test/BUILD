load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")
load("@rules_python//python:defs.bzl", "py_test")

cc_library(
    name = "examples_test_utils",
    hdrs = ["examples_test_utils.h"],
    deps = ["//cyber:cyber_core"],
)

cc_binary(
    name = "cross_process_lifecycle_helper",
    srcs = ["cross_process_lifecycle_helper.cc"],
    deps = [
        "//cyber:cyber_core",
        "//cyber/service_discovery:topology_manager",
        "//examples/proto:examples_cc_proto",
    ],
)

cc_binary(
    name = "librecorder_input_component.so",
    srcs = [
        "recorder_input_component.cc",
        "recorder_input_component.h",
    ],
    linkshared = True,
    linkstatic = False,
    deps = [
        "//cyber:cyber_core",
        "//examples/proto:examples_cc_proto",
    ],
)

cc_test(
    name = "examples_integration_test",
    size = "small",
    srcs = ["examples_integration_test.cc"],
    data = ["//cyber:cyber_conf"],
    deps = [
        "//cyber:cyber_core",
        "//examples/proto:examples_cc_proto",
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
        "//examples/proto:examples_cc_proto",
        ":examples_test_utils",
        "@com_google_googletest//:gtest",
    ],
)

py_test(
    name = "cyber_tools_discovery_test",
    size = "medium",
    timeout = "moderate",
    srcs = ["cyber_tools_discovery_test.py"],
    tags = [
        "exclusive",
        "local",
    ],
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

py_test(
    name = "cross_process_churn_test",
    size = "large",
    timeout = "long",
    srcs = ["cross_process_churn_test.py"],
    tags = [
        "exclusive",
        "local",
    ],
    data = [
        ":cross_process_lifecycle_helper",
        ":librecorder_input_component.so",
        "//cyber:cyber_conf",
        "//cyber/mainboard:mainboard",
        "//cyber/tools/cyber_recorder:cyber_recorder",
    ],
)

py_test(
    name = "mainboard_integration_test",
    size = "small",
    srcs = ["mainboard_integration_test.py"],
    data = [
        "//cyber:cyber_conf",
        "//cyber/mainboard:mainboard",
        "//examples/timer_component_example:libtimer_component_example.so",
    ],
)

cc_test(
    name = "cyber_lifecycle_test",
    size = "small",
    srcs = ["cyber_lifecycle_test.cc"],
    data = ["//cyber:cyber_conf"],
    deps = [
        "//cyber:init",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "core_transport_matrix_test",
    size = "medium",
    timeout = "moderate",
    srcs = ["core_transport_matrix_test.cc"],
    data = ["//cyber:cyber_conf"],
    tags = [
        "exclusive",
        "local",
    ],
    deps = [
        "//cyber:cyber_core",
        "//cyber/proto:unit_test_cc_proto",
        "//cyber/transport/message:pod_message",
        "@com_google_googletest//:gtest",
    ],
)

test_suite(
    name = "examples_regression_tests",
    tests = [
        ":cyber_lifecycle_test",
        ":cyber_tools_discovery_test",
        ":cross_process_churn_test",
        ":examples_integration_test",
        ":examples_stress_test",
        ":mainboard_integration_test",
        "//examples/record_play:record_play_test",
    ],
)

test_suite(
    name = "core_tool_matrix_tests",
    tests = [
        ":core_transport_matrix_test",
        ":examples_regression_tests",
        "//cyber/python/cyber_py3/examples:examples_smoke_test",
        "//cyber/python/cyber_py3/test:cyber_test",
        "//cyber/python/cyber_py3/test:cyber_time_test",
        "//cyber/python/cyber_py3/test:cyber_timer_test",
        "//cyber/python/cyber_py3/test:init_test",
        "//cyber/python/cyber_py3/test:parameter_test",
        "//cyber/python/cyber_py3/test:record_test",
        "//cyber/python/cyber_py3/test:shutdown_exit_test",
        "//cyber/transport:transport_test",
        "//cyber/transport/integration_test:hybrid_transceiver_test",
        "//cyber/transport/integration_test:intra_transceiver_test",
        "//cyber/transport/integration_test:rtps_transceiver_test",
        "//cyber/transport/integration_test:shm_transceiver_test",
        "//cyber/transport/rtps:rtps_test",
        "//examples/record_play:record_play_tool_test",
    ],
)
