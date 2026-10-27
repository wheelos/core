load(":cc_headers.bzl", "cc_sdk_headers")

exports_files(
    ["cc_headers.bzl"],
    visibility = ["//:internal"],
)

cc_sdk_headers(
    name = "application_headers",
    testonly = True,
    targets = ["//cyber"],
)

filegroup(
    name = "application_header_manifest",
    testonly = True,
    srcs = [":application_headers"],
    output_group = "manifest",
    visibility = ["//tests/public_api:__pkg__"],
)

genquery(
    name = "module_dependency_violations",
    expression = "filter('^//cyber/node:', deps(set(//cyber/transport " +
                 "//cyber/transport/nvsci:gpu_channel_manager " +
                 "//cyber/service //cyber/service:client " +
                 "//cyber/blocker:blocker_manager))) union " +
                 "filter('^//cyber/service_discovery[/:]', " +
                 "deps(set(//cyber/transport " +
                 "//cyber/transport/nvsci:gpu_channel_manager)))",
    scope = [
        "//cyber/blocker:blocker_manager",
        "//cyber/service",
        "//cyber/service:client",
        "//cyber/transport",
        "//cyber/transport/nvsci:gpu_channel_manager",
    ],
    visibility = ["//tests/public_api:__pkg__"],
)

genquery(
    name = "api_visibility",
    testonly = True,
    expression = "visible(//tests/public_api:cpp_headers, set(" +
                 "//cyber " +
                 "//cyber/node " +
                 "//cyber/service " +
                 "//cyber/service:client " +
                 "//cyber/gpu:gpu_reader " +
                 "//cyber/gpu:gpu_writer " +
                 "//cyber/node:node_channel_impl " +
                 "//cyber/node:intra_reader " +
                 "//cyber/scheduler " +
                 "//cyber/service_discovery:topology_manager " +
                 "//cyber/transport " +
                 "//cyber/transport/nvsci:gpu_channel_manager " +
                 "//cyber/gpu:gpu_control_endpoint " +
                 "//cyber/python/internal:_cyber_wrapper))",
    scope = [
        "//tests/public_api:cpp_headers",
        "//cyber",
        "//cyber/gpu:gpu_reader",
        "//cyber/gpu:gpu_writer",
        "//cyber/python/internal:_cyber_wrapper",
    ],
    visibility = ["//tests/public_api:__pkg__"],
)
