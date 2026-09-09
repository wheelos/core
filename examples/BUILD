load("@rules_cc//cc:defs.bzl", "cc_binary")

package(default_visibility = ["//visibility:public"])

cc_binary(
    name = "talker",
    srcs = ["talker.cc"],
    deps = [
        "//cyber",
        "//examples/proto:examples_cc_proto",
    ],
)

cc_binary(
    name = "listener",
    srcs = ["listener.cc"],
    deps = [
        "//cyber",
        "//examples/proto:examples_cc_proto",
    ],
)

cc_binary(
    name = "talker_fb",
    srcs = ["talker_fb.cc"],
    deps = [
        "//cyber",
        "//cyber/message:flatbuffers_message",
        "//examples/proto:examples_flatbuffers",
    ],
)

cc_binary(
    name = "listener_fb",
    srcs = ["listener_fb.cc"],
    deps = [
        "//cyber",
        "//cyber/message:flatbuffers_message",
        "//examples/proto:examples_flatbuffers",
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
        "//examples/proto:examples_cc_proto",
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

cc_library(
    name = "gpu_image_meta",
    hdrs = ["gpu_image_meta.h"],
)

cc_binary(
    name = "gpu_talker",
    srcs = ["gpu_talker.cc"],
    linkopts = ["-lcudart"],
    deps = [
        ":gpu_image_meta",
        "//cyber",
        "//cyber/transport/nvsci:gpu_writer",
    ],
)

cc_binary(
    name = "gpu_listener",
    srcs = ["gpu_listener.cc"],
    linkopts = ["-lcudart"],
    deps = [
        ":gpu_image_meta",
        "//cyber",
        "//cyber/transport/nvsci:gpu_reader",
    ],
)
