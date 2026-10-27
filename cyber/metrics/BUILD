load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

package(default_visibility = ["//:internal"])

cc_library(
    name = "metrics",
    srcs = ["metrics.cc"],
    hdrs = ["metrics.h"],
)

cc_test(
    name = "metrics_test",
    size = "small",
    srcs = ["metrics_test.cc"],
    deps = [
        ":metrics",
        "@com_google_googletest//:gtest_main",
    ],
)
