load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "client",
    hdrs = ["client.h"],
    deps = [
        ":client_base",
        "//cyber/common:log",
        "//cyber/common:types",
        "//cyber/node:node_channel_impl",
    ],
)

cc_library(
    name = "client_base",
    hdrs = ["client_base.h"],
)

cc_library(
    name = "service",
    hdrs = ["service.h"],
    deps = [
        ":service_base",
        "//cyber/scheduler",
    ],
)

cc_library(
    name = "service_base",
    hdrs = ["service_base.h"],
)

cc_test(
    name = "client_test",
    size = "small",
    srcs = ["client_test.cc"],
    deps = [
        ":client",
        "@com_google_googletest//:gtest_main",
    ],
)
