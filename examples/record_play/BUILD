load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "record_play",
    hdrs = ["record_play.h"],
    deps = [
        "//cyber",
        "//cyber/record:record_reader",
        "//cyber/transport/message:pod_message",
    ],
)

cc_binary(
    name = "record_play_example",
    srcs = ["record_play.cc"],
    deps = [
        ":record_play",
        "//cyber:init",
    ],
)

cc_library(
    name = "record_play_tool",
    hdrs = ["record_play_tool.h"],
    deps = [
        ":record_play",
        "//cyber/message:protobuf_factory",
        "//cyber/record:record_reader",
        "//cyber/record:record_writer",
        "//cyber/transport/message:pod_message",
    ],
)

cc_binary(
    name = "record_play_tool_example",
    srcs = ["record_play_tool.cc"],
    deps = [
        ":record_play_tool",
        "//cyber",
        "//cyber:init",
    ],
)

cc_binary(
    name = "record_play_channels_dump",
    srcs = ["record_play_channels_dump.cc"],
    deps = [
        "//cyber/record:record_reader",
    ],
)

cc_binary(
    name = "pod_image_publisher",
    srcs = ["pod_image_publisher.cc"],
    deps = [
        "//cyber",
        "//cyber:init",
        "//cyber/transport/message:pod_message",
    ],
)

cc_binary(
    name = "pod_image_subscriber",
    srcs = ["pod_image_subscriber.cc"],
    deps = [
        "//cyber",
        "//cyber:init",
        "//cyber/transport/message:pod_message",
    ],
)

cc_test(
    name = "record_play_tool_test",
    size = "small",
    srcs = ["record_play_tool_test.cc"],
    deps = [
        ":record_play_tool",
        "//cyber:init",
        "@com_google_protobuf//:protobuf",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "record_play_test",
    size = "medium",
    srcs = ["record_play_test.cc"],
    data = ["testdata/cyber/conf/cyber.pb.conf"],
    tags = ["local"],
    deps = [
        ":record_play",
        "//cyber:init",
        "@com_google_googletest//:gtest",
    ],
)
