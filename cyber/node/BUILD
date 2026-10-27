load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

package(default_visibility = ["//:internal"])

cc_library(
    name = "node",
    srcs = ["node.cc"],
    hdrs = ["node.h"],
    implementation_deps = [
        "//cyber:state",
        "//cyber/common:global_data",
        "//cyber/time",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":node_channel_impl",
        ":node_service_impl",
    ],
)

cc_library(
    name = "node_channel_impl",
    hdrs = ["node_channel_impl.h"],
    deps = [
        ":intra_reader",
        ":intra_writer",
        ":reader",
        ":writer",
        "//cyber/common:global_data",
        "//cyber/message:message_traits",
        "//cyber/proto:run_mode_conf_cc_proto",
    ],
)

cc_test(
    name = "node_channel_impl_test",
    size = "small",
    srcs = ["node_channel_impl_test.cc"],
    deps = [
        "//cyber",
        "//cyber/proto:unit_test_cc_proto",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "intra_reader",
    hdrs = ["internal/intra_reader.h"],
    deps = [
        ":reader",
        "//cyber/blocker:blocker_manager",
        "//cyber/common:log",
        "//cyber/time",
    ],
)

cc_library(
    name = "intra_writer",
    hdrs = ["internal/intra_writer.h"],
    deps = [
        ":writer",
        "//cyber/blocker:blocker_manager",
    ],
)

cc_test(
    name = "intra_reader_writer_test",
    size = "small",
    srcs = ["intra_reader_writer_test.cc"],
    deps = [
        ":intra_reader",
        ":intra_writer",
        "//cyber/proto:unit_test_cc_proto",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "node_service_impl",
    hdrs = ["node_service_impl.h"],
    deps = [
        ":reader",
        ":writer",
        "//cyber/common:global_data",
        "//cyber/service",
        "//cyber/service:client",
        "//cyber/service_discovery:topology_manager",
    ],
)

cc_test(
    name = "node_test",
    size = "small",
    srcs = ["node_test.cc"],
    deps = [
        "//cyber",
        "//cyber/proto:unit_test_cc_proto",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "reader",
    hdrs = ["reader.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":reader_base",
        "//cyber/blocker",
        "//cyber/common:global_data",
        "//cyber/croutine:routine_factory",
        "//cyber/data:data_visitor",
        "//cyber/metrics",
        "//cyber/proto:topology_change_cc_proto",
        "//cyber/scheduler",
        "//cyber/service_discovery:topology_manager",
        "//cyber/time",
        "//cyber/transport",
    ],
)

cc_library(
    name = "reader_base",
    hdrs = ["reader_base.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//cyber/event:perf_event_cache",
        "//cyber/metrics",
        "//cyber/transport",
    ],
)

cc_test(
    name = "reader_test",
    size = "small",
    srcs = ["reader_test.cc"],
    deps = [
        "//cyber",
        "//cyber/proto:unit_test_cc_proto",
        "@com_google_googletest//:gtest",
    ],
)

cc_library(
    name = "writer",
    hdrs = ["writer.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":writer_base",
        "//cyber/common:log",
        "//cyber/metrics",
        "//cyber/proto:topology_change_cc_proto",
        "//cyber/service_discovery:topology_manager",
        "//cyber/transport",
    ],
)

cc_library(
    name = "writer_base",
    hdrs = ["writer_base.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//cyber/common:macros",
        "//cyber/common:util",
        "//cyber/proto:role_attributes_cc_proto",
    ],
)

cc_test(
    name = "writer_reader_test",
    size = "small",
    srcs = ["writer_reader_test.cc"],
    deps = [
        "//cyber",
        "//cyber/proto:unit_test_cc_proto",
        "@com_google_googletest//:gtest",
    ],
)

cc_test(
    name = "writer_test",
    size = "small",
    srcs = ["writer_test.cc"],
    deps = [
        "//cyber",
        "//cyber/node:writer_base",
        "//cyber/proto:unit_test_cc_proto",
        "@com_google_googletest//:gtest",
    ],
)
