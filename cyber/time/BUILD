load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

package(default_visibility = ["//:internal"])

cc_library(
    name = "time",
    srcs = ["time.cc"],
    hdrs = ["time.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":duration",
    ],
)

cc_test(
    name = "time_test",
    size = "small",
    srcs = ["time_test.cc"],
    deps = [
        ":time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "rate",
    srcs = ["rate.cc"],
    hdrs = ["rate.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":duration",
        ":time",
        "//cyber/common:log",
    ],
)

cc_library(
    name = "duration",
    srcs = ["duration.cc"],
    hdrs = ["duration.h"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "duration_test",
    size = "small",
    srcs = ["duration_test.cc"],
    deps = [
        ":duration",
        ":time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "clock",
    srcs = ["clock.cc"],
    hdrs = ["clock.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":time",
        "//cyber/base:atomic_rw_lock",
        "//cyber/common:global_data",
        "//cyber/common:log",
        "//cyber/common:macros",
        "//cyber/common:util",
        "//cyber/proto:run_mode_conf_cc_proto",
    ],
)

cc_test(
    name = "clock_test",
    size = "small",
    srcs = ["clock_test.cc"],
    deps = [
        ":clock",
        "@com_google_googletest//:gtest_main",
    ],
)
