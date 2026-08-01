load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "action_context",
    hdrs = ["action_context.h"],
)

cc_library(
    name = "action_types",
    hdrs = ["action_types.h"],
)

cc_test(
    name = "action_test",
    size = "small",
    srcs = ["action_test.cc"],
    deps = [
        ":action_context",
        ":action_types",
        "@com_google_googletest//:gtest_main",
    ],
)
