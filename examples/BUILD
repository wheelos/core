load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")
load("@rules_cuda//cuda:defs.bzl", "cuda_library", "requires_cuda")

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

cuda_library(
    name = "gpu_classifier_kernel",
    srcs = ["gpu_classifier_kernel.cu"],
    hdrs = ["gpu_classifier_kernel.h"],
    features = ["-nvcc_extended_lambda"],
    target_compatible_with = requires_cuda(),
    deps = ["@cuda//:cuda_runtime"],
)

cc_binary(
    name = "gpu_talker",
    srcs = ["gpu_talker.cc"],
    target_compatible_with = requires_cuda(),
    deps = [
        ":gpu_image_meta",
        "//cyber",
        "//cyber/gpu:gpu_writer",
        "@cuda//:cuda_runtime",
    ],
)

cc_binary(
    name = "gpu_listener",
    srcs = ["gpu_listener.cc"],
    target_compatible_with = requires_cuda(),
    deps = [
        ":gpu_image_meta",
        "//cyber",
        "//cyber/gpu:gpu_reader",
        "@cuda//:cuda_runtime",
    ],
)

cc_binary(
    name = "gpu_inference_talker",
    srcs = ["gpu_inference_talker.cc"],
    target_compatible_with = requires_cuda(),
    deps = [
        ":gpu_image_meta",
        "//cyber",
        "//cyber/gpu:gpu_writer",
        "@cuda//:cuda_runtime",
    ],
)

cc_binary(
    name = "gpu_inference_listener",
    srcs = ["gpu_inference_listener.cc"],
    target_compatible_with = requires_cuda(),
    deps = [
        ":gpu_classifier_kernel",
        ":gpu_image_meta",
        "//cyber",
        "//cyber/gpu:gpu_reader",
        "@cuda//:cuda_runtime",
    ],
)

cc_test(
    name = "gpu_classifier_kernel_test",
    srcs = ["gpu_classifier_kernel_test.cc"],
    target_compatible_with = requires_cuda(),
    deps = [
        ":gpu_classifier_kernel",
        "@com_google_googletest//:gtest_main",
        "@cuda//:cuda_runtime",
    ],
)
