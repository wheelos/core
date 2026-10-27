load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

package(default_visibility = ["//:internal"])

cc_library(
    name = "blocker_manager",
    srcs = ["blocker_manager.cc"],
    hdrs = ["blocker_manager.h"],
    deps = [
        ":blocker",
    ],
)

cc_test(
    name = "blocker_manager_test",
    size = "small",
    srcs = ["blocker_manager_test.cc"],
    deps = [
        ":blocker_manager",
        "//cyber/proto:unit_test_cc_proto",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "blocker",
    hdrs = ["blocker.h"],
)

cc_test(
    name = "blocker_test",
    size = "small",
    srcs = ["blocker_test.cc"],
    deps = [
        ":blocker",
        "//cyber/proto:unit_test_cc_proto",
        "@com_google_googletest//:gtest_main",
    ],
)
