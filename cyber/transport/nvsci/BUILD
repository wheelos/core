load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

package(default_visibility = ["//visibility:public"])

config_setting(
    name = "use_nvsci_enabled",
    values = {"define": "use_nvsci=true"},
)

cc_library(
    name = "nvsci_types",
    hdrs = ["nvsci_types.h"],
)

cc_library(
    name = "gpu_wire",
    hdrs = ["gpu_wire.h"],
)

cc_library(
    name = "orin_nvsci_backend",
    srcs = ["orin_nvsci_backend.cc"],
    hdrs = [
        "orin_nvsci_backend.h",
        "orin_nvsci_wrapper.h",
    ],
    defines = select({
        ":use_nvsci_enabled": ["CYBER_USE_NVSCI=1"],
        "//conditions:default": ["CYBER_USE_CUDA_IPC=1"],
    }),
    deps = [
        ":nvsci_types",
        "//cyber/common:log",
    ],
)

cc_library(
    name = "nvsci_buf_pool",
    srcs = ["nvsci_buf_pool.cc"],
    hdrs = ["nvsci_buf_pool.h"],
    defines = select({
        ":use_nvsci_enabled": ["CYBER_USE_NVSCI=1"],
        "//conditions:default": ["CYBER_USE_CUDA_IPC=1"],
    }),
    linkopts = select({
        ":use_nvsci_enabled": [
            "-lnvscibuf",
            "-lnvscisync",
            "-lrt",
            "-lcudart",
        ],
        "//conditions:default": [
            "-lrt",
            "-lcudart",
        ],
    }),
    deps = [
        ":gpu_wire",
        ":nvsci_types",
        ":orin_nvsci_backend",
        "//cyber/common:log",
    ],
)

cc_library(
    name = "nvsci_sync_engine",
    srcs = ["nvsci_sync_engine.cc"],
    hdrs = ["nvsci_sync_engine.h"],
    defines = select({
        ":use_nvsci_enabled": ["CYBER_USE_NVSCI=1"],
        "//conditions:default": ["CYBER_USE_CUDA_IPC=1"],
    }),
    linkopts = select({
        ":use_nvsci_enabled": [
            "-lnvscibuf",
            "-lnvscisync",
            "-lrt",
            "-lcudart",
        ],
        "//conditions:default": [
            "-lrt",
            "-lcudart",
        ],
    }),
    deps = [
        ":gpu_wire",
        ":nvsci_types",
        ":orin_nvsci_backend",
    ],
)

cc_library(
    name = "gpu_channel_session",
    srcs = ["gpu_channel_session.cc"],
    hdrs = ["gpu_channel_session.h"],
    deps = [
        ":nvsci_buf_pool",
        ":nvsci_sync_engine",
        ":nvsci_types",
    ],
)

cc_library(
    name = "gpu_loaned_message",
    hdrs = ["gpu_loaned_message.h"],
    deps = [
        ":gpu_channel_session",
        ":nvsci_types",
    ],
)

cc_library(
    name = "gpu_control_protocol",
    hdrs = ["gpu_control_protocol.h"],
    deps = [
        ":gpu_wire",
        ":nvsci_types",
        "@com_google_protobuf//:protobuf",
    ],
)

cc_library(
    name = "gpu_control_endpoint",
    hdrs = ["gpu_control_endpoint.h"],
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
    name = "gpu_channel_manager",
    srcs = ["gpu_channel_manager.cc"],
    hdrs = ["gpu_channel_manager.h"],
    deps = [
        ":gpu_wire",
        ":gpu_channel_session",
        ":nvsci_buf_pool",
        ":nvsci_sync_engine",
        "//cyber/common:macros",
        "//cyber/common:util",
    ],
)

cc_library(
    name = "gpu_writer",
    hdrs = ["gpu_writer.h"],
    deps = [
        ":gpu_control_endpoint",
        ":gpu_channel_manager",
        ":gpu_channel_session",
        ":gpu_control_protocol",
        ":nvsci_types",
        "//cyber/common:log",
        "//cyber/message:raw_message",
        "//cyber/node",
    ],
)

cc_library(
    name = "gpu_reader",
    hdrs = ["gpu_reader.h"],
    deps = [
        ":gpu_control_endpoint",
        ":gpu_channel_manager",
        ":gpu_channel_session",
        ":gpu_control_protocol",
        ":nvsci_types",
        "//cyber/common:log",
        "//cyber/common:util",
        "//cyber/message:raw_message",
        "//cyber/node",
    ],
)

cc_test(
    name = "gpu_writer_reader_test",
    size = "small",
    srcs = ["gpu_writer_reader_test.cc"],
    linkopts = ["-lcudart"],
    deps = [
        ":gpu_reader",
        ":gpu_writer",
        "//cyber",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "gpu_control_protocol_test",
    size = "small",
    srcs = ["gpu_control_protocol_test.cc"],
    deps = [
        ":gpu_control_protocol",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "nvsci_buf_pool_test",
    size = "small",
    srcs = ["nvsci_buf_pool_test.cc"],
    deps = [
        ":nvsci_buf_pool",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "nvsci_sync_engine_test",
    size = "small",
    srcs = ["nvsci_sync_engine_test.cc"],
    deps = [
        ":nvsci_sync_engine",
        "@com_google_googletest//:gtest",
    ],
)

cc_test(
    name = "gpu_channel_session_test",
    size = "small",
    srcs = ["gpu_channel_session_test.cc"],
    deps = [
        ":gpu_channel_session",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "gpu_loaned_message_test",
    size = "small",
    srcs = ["gpu_loaned_message_test.cc"],
    deps = [
        ":gpu_loaned_message",
        "@com_google_googletest//:gtest_main",
    ],
)
