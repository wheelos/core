load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "client",
    hdrs = ["client.h"],
    deps = [
        ":client_base",
        ":rpc_options",
        ":rpc_status",
        "//cyber/proto:rpc_cc_proto",
    ],
)

cc_library(
    name = "client_base",
    hdrs = ["client_base.h"],
    deps = [
        "//cyber:state",
        "//cyber/service_discovery:topology_manager",
    ],
)

cc_library(
    name = "rpc_context",
    hdrs = ["rpc_context.h"],
)

cc_library(
    name = "rpc_options",
    hdrs = ["rpc_options.h"],
)

cc_library(
    name = "rpc_status",
    hdrs = ["rpc_status.h"],
)

cc_library(
    name = "service",
    hdrs = ["service.h"],
    deps = [
        ":rpc_options",
        ":rpc_context",
        ":rpc_status",
        ":service_base",
        "//cyber/proto:rpc_cc_proto",
        "//cyber/scheduler",
    ],
)

cc_library(
    name = "service_base",
    hdrs = ["service_base.h"],
)

cc_test(
    name = "rpc_test",
    size = "small",
    srcs = ["rpc_test.cc"],
    deps = [
        ":rpc_context",
        ":rpc_status",
        "@com_google_googletest//:gtest_main",
    ],
)
