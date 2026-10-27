load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")
load("@rules_python//python:defs.bzl", "py_test")

cc_library(
    name = "cpp_headers",
    testonly = True,
    srcs = [
        "client_header_compile.cc",
        "gpu_header_compile.cc",
        "recorder_header_compile.cc",
        "service_header_compile.cc",
    ],
    visibility = ["//tools/sdk:__pkg__"],
    deps = [
        "//cyber/gpu:gpu_reader",
        "//cyber/gpu:gpu_writer",
        "//cyber/node",
        "//cyber/proto:parameter_cc_proto",
        "//cyber/service",
        "//cyber/service:client",
        "//cyber/tools/cyber_recorder:recorder",
    ],
)

cc_test(
    name = "node_public_api_test",
    size = "small",
    srcs = ["node_public_api_test.cc"],
    data = ["//cyber:cyber_conf"],
    deps = [
        "//cyber:init",
        "//cyber/node",
        "//cyber/proto:parameter_cc_proto",
        "@com_google_googletest//:gtest_main",
    ],
)

py_test(
    name = "sdk_contract_test",
    size = "small",
    srcs = ["sdk_contract_test.py"],
    args = [
        "$(rootpath //:wheelos_cyber_header_manifest)",
        "$(rootpath //:wheelos_cyber_public_header_manifest)",
        "$(rootpath //tools/sdk:api_visibility)",
        "$(rootpath //:wheelos_cyber_sdk)",
        "$(rootpath //tools/sdk:application_header_manifest)",
        "$(rootpath //tools/sdk:module_dependency_violations)",
    ],
    data = [
        ":cpp_headers",
        "//:wheelos_cyber_header_manifest",
        "//:wheelos_cyber_headers",
        "//:wheelos_cyber_public_header_manifest",
        "//:wheelos_cyber_sdk",
        "//tools/sdk:api_visibility",
        "//tools/sdk:application_header_manifest",
        "//tools/sdk:module_dependency_violations",
    ],
)
