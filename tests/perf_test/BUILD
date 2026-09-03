load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_test")
load("@rules_python//python:defs.bzl", "py_test")

package(default_visibility = ["//visibility:public"])

cc_binary(
    name = "benchmark_monitor",
    srcs = [
        "benchmark_measurement_window.h",
        "benchmark_rate_acceptance.h",
        "benchmark_sequence_tracker.h",
        "cyber_rt_benchmark_suite.cc",
        "fanout_validation.h",
    ],
    data = [
        ":benchmark_pub",
        ":benchmark_roudi",
        ":benchmark_sub",
        "//cyber:cyber_conf",
    ],
    deps = [
        "//cyber:cyber_core",
        "//cyber/proto:unit_test_cc_proto",
        "//cyber/transport/message:pod_message",
    ],
)

cc_test(
    name = "benchmark_sequence_tracker_test",
    srcs = [
        "benchmark_sequence_tracker.h",
        "benchmark_sequence_tracker_test.cc",
    ],
    deps = [
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "benchmark_measurement_window_test",
    srcs = [
        "benchmark_measurement_window.h",
        "benchmark_measurement_window_test.cc",
        "benchmark_sequence_tracker.h",
    ],
    deps = [
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "benchmark_rate_acceptance_test",
    srcs = [
        "benchmark_rate_acceptance.h",
        "benchmark_rate_acceptance_test.cc",
    ],
    deps = [
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "fanout_validation_test",
    srcs = [
        "fanout_validation.h",
        "fanout_validation_test.cc",
    ],
    deps = [
        "@com_google_googletest//:gtest_main",
    ],
)

cc_binary(
    name = "benchmark_pub",
    srcs = [
        "benchmark_process_common.h",
        "benchmark_pub.cc",
    ],
    data = ["//cyber:cyber_conf"],
    deps = [
        "//cyber:cyber_core",
        "//cyber/proto:unit_test_cc_proto",
        "//cyber/transport/message:pod_message",
    ],
)

cc_binary(
    name = "benchmark_sub",
    srcs = [
        "benchmark_measurement_window.h",
        "benchmark_process_common.h",
        "benchmark_sequence_tracker.h",
        "benchmark_sub.cc",
    ],
    data = ["//cyber:cyber_conf"],
    deps = [
        "//cyber:cyber_core",
        "//cyber/proto:unit_test_cc_proto",
        "//cyber/transport/message:pod_message",
    ],
)

cc_binary(
    name = "benchmark_roudi",
    srcs = ["benchmark_roudi.cc"],
    deps = [
        "@iceoryx//:iceoryx_posh",
        "@iceoryx//iceoryx_posh:iceoryx_posh_roudi",
    ],
)

cc_binary(
    name = "cyber_rt_benchmark_suite",
    srcs = [
        "benchmark_measurement_window.h",
        "benchmark_rate_acceptance.h",
        "benchmark_sequence_tracker.h",
        "cyber_rt_benchmark_suite.cc",
        "fanout_validation.h",
    ],
    data = [
        ":benchmark_pub",
        ":benchmark_roudi",
        ":benchmark_sub",
        "//cyber:cyber_conf",
    ],
    deps = [
        "//cyber:cyber_core",
        "//cyber/proto:unit_test_cc_proto",
        "//cyber/transport/message:pod_message",
    ],
)

py_test(
    name = "iceoryx_roudi_lifecycle_test",
    size = "large",
    timeout = "long",
    srcs = ["iceoryx_roudi_lifecycle_test.py"],
    tags = [
        "exclusive",
        "local",
    ],
    data = [
        ":benchmark_pub",
        ":benchmark_roudi",
        ":benchmark_sub",
        ":cyber_rt_benchmark_suite",
        "//cyber:cyber_conf",
    ],
)
