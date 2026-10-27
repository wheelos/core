load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

package(default_visibility = ["//:internal"])

cc_library(
    name = "client",
    hdrs = ["client.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":client_base",
        "//cyber/common:global_data",
        "//cyber/common:log",
        "//cyber/common:types",
        "//cyber/transport",
    ],
)

cc_library(
    name = "client_base",
    srcs = ["client_base.cc"],
    hdrs = ["client_base.h"],
    implementation_deps = ["//cyber/service_discovery:topology_manager"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "service",
    hdrs = ["service.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":service_base",
        "//cyber:state",
        "//cyber/common:global_data",
        "//cyber/common:log",
        "//cyber/common:types",
        "//cyber/transport",
    ],
)

cc_library(
    name = "service_base",
    hdrs = ["service_base.h"],
    visibility = ["//visibility:public"],
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
