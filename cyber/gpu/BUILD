load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")
load("@rules_cuda//cuda:defs.bzl", "requires_cuda")

package(default_visibility = ["//:internal"])

cc_library(
    name = "gpu_control_endpoint",
    hdrs = ["internal/gpu_control_endpoint.h"],
    deps = [
        "//cyber/common:global_data",
        "//cyber/common:util",
        "//cyber/message:message_traits",
        "//cyber/message:protobuf_factory",
        "//cyber/message:raw_message",
        "//cyber/node",
        "//cyber/transport/common:identity",
    ],
)

cc_library(
    name = "gpu_writer",
    hdrs = ["gpu_writer.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":gpu_control_endpoint",
        "//cyber/common:log",
        "//cyber/message:raw_message",
        "//cyber/node",
        "//cyber/transport/nvsci:gpu_channel_manager",
        "//cyber/transport/nvsci:gpu_channel_session",
        "//cyber/transport/nvsci:gpu_control_protocol",
        "//cyber/transport/nvsci:nvsci_types",
    ],
)

cc_library(
    name = "gpu_reader",
    hdrs = ["gpu_reader.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":gpu_control_endpoint",
        "//cyber/common:log",
        "//cyber/common:util",
        "//cyber/message:raw_message",
        "//cyber/node",
        "//cyber/transport/nvsci:gpu_channel_manager",
        "//cyber/transport/nvsci:gpu_channel_session",
        "//cyber/transport/nvsci:gpu_control_protocol",
        "//cyber/transport/nvsci:nvsci_types",
    ],
)

cc_test(
    name = "gpu_writer_reader_test",
    size = "small",
    srcs = ["gpu_writer_reader_test.cc"],
    target_compatible_with = requires_cuda(),
    deps = [
        ":gpu_reader",
        ":gpu_writer",
        "//cyber",
        "@com_google_googletest//:gtest_main",
    ],
)
