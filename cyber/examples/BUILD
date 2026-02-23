load("@rules_cc//cc:defs.bzl", "cc_binary")

package(default_visibility = ["//visibility:public"])

cc_binary(
    name = "talker",
    srcs = ["talker.cc"],
    deps = [
        "//cyber",
        "//cyber/examples/proto:examples_cc_proto",
    ],
)

cc_binary(
    name = "listener",
    srcs = ["listener.cc"],
    deps = [
        "//cyber",
        "//cyber/examples/proto:examples_cc_proto",
    ],
)

cc_binary(
    name = "paramserver",
    srcs = ["paramserver.cc"],
    deps = [
        "//cyber",
        "//cyber/parameter",
    ],
)

cc_binary(
    name = "service",
    srcs = ["service.cc"],
    deps = [
        "//cyber",
        "//cyber/examples/proto:examples_cc_proto",
    ],
)

cc_binary(
    name = "record",
    srcs = ["record.cc"],
    deps = [
        "//cyber",
        "//cyber/proto:record_cc_proto",
    ],
)

# FlatBuffers zero-copy talker/listener pair.
cc_binary(
    name = "talker_fb",
    srcs = ["talker_fb.cc"],
    deps = [
        "//cyber",
        "//cyber/examples/proto:examples_fbs",
        "//cyber/message:flatbuffers_message",
    ],
)

cc_binary(
    name = "listener_fb",
    srcs = ["listener_fb.cc"],
    deps = [
        "//cyber",
        "//cyber/examples/proto:examples_fbs",
        "//cyber/message:flatbuffers_message",
    ],
)
