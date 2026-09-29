load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "transport",
    srcs = ["transport.cc"],
    hdrs = ["transport.h"],
    deps = [
        ":iceoryx_runtime",
        "//cyber/service_discovery:role",
        "//cyber/task",
        "//cyber/transport/dispatcher:intra_dispatcher",
        "//cyber/transport/dispatcher:rtps_dispatcher",
        "//cyber/transport/dispatcher:shm_dispatcher",
        "//cyber/transport/message:history",
        "//cyber/transport/qos",
        "//cyber/transport/receiver",
        "//cyber/transport/rtps:attributes_filler",
        "//cyber/transport/rtps:participant",
        "//cyber/transport/rtps:sub_listener",
        "//cyber/transport/rtps:underlay_message",
        "//cyber/transport/rtps:underlay_message_type",
        "//cyber/transport/shm:profile",
        "//cyber/transport/transmitter",
        "@fastdds",
    ],
)

cc_library(
    name = "iceoryx_runtime",
    hdrs = ["iceoryx_runtime.h"],
    deps = [
        "//cyber/common:global_data",
        "//cyber/common:util",
        "//cyber/proto:role_attributes_cc_proto",
        "@iceoryx//:iceoryx_posh",
        "@iceoryx//iceoryx_posh:iceoryx_posh_roudi",
    ],
)

cc_library(
    name = "iceoryx_chunk",
    hdrs = ["iceoryx_chunk.h"],
    deps = [
        "//cyber/message:message_traits",
    ],
)

cc_test(
    name = "transport_test",
    size = "small",
    srcs = ["transport_test.cc"],
    tags = [
        "exclusive",
        "local",
    ],
    deps = [
        "//cyber:cyber_core",
        "//cyber/proto:unit_test_cc_proto",
        "@com_google_googletest//:gtest",
    ],
)
