load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "participant_listener",
    srcs = ["participant_listener.cc"],
    hdrs = ["participant_listener.h"],
    deps = [
        "//cyber/common:log",
        "@wheelos_fastdds//:fastdds",
    ],
)

cc_library(
    name = "subscriber_listener",
    srcs = ["subscriber_listener.cc"],
    hdrs = ["subscriber_listener.h"],
    deps = [
        "//cyber/common:log",
        "//cyber/transport/rtps:underlay_message",
        "//cyber/transport/rtps:underlay_message_type",
        "@wheelos_fastdds//:fastdds",
    ],
)
