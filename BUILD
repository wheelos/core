load("@bazel_skylib//lib:selects.bzl", "selects")
load("@rules_pkg//:pkg.bzl", "pkg_deb", "pkg_tar")
load("@rules_pkg//pkg:mappings.bzl", "pkg_attributes", "pkg_files", "strip_prefix")
load("//tools/sdk:cc_headers.bzl", "cc_sdk_headers")

package(default_visibility = ["//:internal"])

package_group(
    name = "internal",
    packages = [
        "-//tests/public_api/...",
        "//",
        "//cyber/...",
        "//examples/...",
        "//packaging/...",
        "//scripts/...",
        "//tests/...",
        "//tools/...",
    ],
)

config_setting(
    name = "cuda_enabled",
    values = {"define": "with_cuda=true"},
)

config_setting(
    name = "cuda_ipc_enabled",
    values = {"define": "use_cuda_ipc=true"},
)

config_setting(
    name = "nvsci_enabled",
    values = {"define": "use_nvsci=true"},
)

selects.config_setting_group(
    name = "gpu_platform_enabled",
    match_any = [
        ":cuda_enabled",
        ":nvsci_enabled",
    ],
)

cc_sdk_headers(
    name = "wheelos_cyber_headers",
    targets = [
        "//cyber:api",
        "//cyber:init",
        "//cyber:state",
        "//cyber/common:file",
        "//cyber/common:log",
        "//cyber/common:macros",
        "//cyber/common:resource_manager",
        "//cyber/component",
        "//cyber/component:component_base",
        "//cyber/component:timer_component",
        "//cyber/gpu:gpu_reader",
        "//cyber/gpu:gpu_writer",
        "//cyber/message:flatbuffers_message",
        "//cyber/message:raw_message",
        "//cyber/node",
        "//cyber/node:reader",
        "//cyber/node:reader_base",
        "//cyber/node:writer",
        "//cyber/node:writer_base",
        "//cyber/parameter",
        "//cyber/parameter:parameter_client",
        "//cyber/parameter:parameter_server",
        "//cyber/proto:clock_cc_proto",
        "//cyber/proto:component_conf_cc_proto",
        "//cyber/proto:dag_conf_cc_proto",
        "//cyber/proto:parameter_cc_proto",
        "//cyber/proto:qos_profile_cc_proto",
        "//cyber/proto:record_cc_proto",
        "//cyber/proto:role_attributes_cc_proto",
        "//cyber/proto:run_mode_conf_cc_proto",
        "//cyber/record:header_builder",
        "//cyber/record:record_base",
        "//cyber/record:record_file_reader",
        "//cyber/record:record_message",
        "//cyber/record:record_reader",
        "//cyber/record:record_viewer",
        "//cyber/record:record_writer",
        "//cyber/service",
        "//cyber/service:client",
        "//cyber/service:client_base",
        "//cyber/service:service_base",
        "//cyber/task",
        "//cyber/time",
        "//cyber/time:clock",
        "//cyber/time:duration",
        "//cyber/time:rate",
        "//cyber/timer",
        "//cyber/tools/cyber_recorder:recorder",
    ],
    visibility = ["//visibility:public"],
)

filegroup(
    name = "wheelos_cyber_public_headers",
    srcs = [":wheelos_cyber_headers"],
    output_group = "public_headers",
    visibility = ["//visibility:public"],
)

filegroup(
    name = "wheelos_cyber_header_manifest",
    srcs = [":wheelos_cyber_headers"],
    output_group = "manifest",
    visibility = ["//tests/public_api:__pkg__"],
)

filegroup(
    name = "wheelos_cyber_public_header_manifest",
    srcs = [":wheelos_cyber_headers"],
    output_group = "public_manifest",
    visibility = ["//tests/public_api:__pkg__"],
)

pkg_files(
    name = "wheelos_cyber_header_files",
    srcs = [":wheelos_cyber_headers"],
    prefix = "/usr/local/include",
    strip_prefix = strip_prefix.from_root(),
)

pkg_files(
    name = "wheelos_cyber_api_manifest_files",
    srcs = [
        ":wheelos_cyber_header_manifest",
        ":wheelos_cyber_public_header_manifest",
    ],
    prefix = "/usr/local/share/wheelos_core",
    strip_prefix = strip_prefix.files_only(),
)

pkg_tar(
    name = "wheelos_cyber_api_manifest",
    srcs = [":wheelos_cyber_api_manifest_files"],
)

pkg_files(
    name = "wheelos_cyber_runtime_library",
    srcs = ["//cyber:cyber_core"],
    attributes = pkg_attributes(mode = "0755"),
    prefix = "/opt/wheelos_core/lib",
    strip_prefix = strip_prefix.files_only(),
)

pkg_files(
    name = "wheelos_cyber_runtime_native_tools",
    srcs = ["//cyber:runtime_native_tools"],
    attributes = pkg_attributes(mode = "0755"),
    include_runfiles = True,
    prefix = "/opt/wheelos_core/bin",
    strip_prefix = strip_prefix.files_only(),
)

pkg_files(
    name = "wheelos_cyber_runtime_launcher",
    srcs = ["scripts/runtime/cyber_launch"],
    attributes = pkg_attributes(mode = "0755"),
    prefix = "/opt/wheelos_core/bin",
    strip_prefix = strip_prefix.files_only(),
)

pkg_files(
    name = "wheelos_cyber_runtime_launcher_source",
    srcs = ["//cyber/tools/cyber_launch:cyber_launch.py"],
    prefix = "/opt/wheelos_core/libexec",
    strip_prefix = strip_prefix.files_only(),
)

pkg_files(
    name = "wheelos_cyber_runtime_setup",
    srcs = ["scripts/runtime/setup.bash"],
    attributes = pkg_attributes(mode = "0755"),
    prefix = "/opt/wheelos_core",
    strip_prefix = strip_prefix.files_only(),
)

pkg_files(
    name = "wheelos_cyber_runtime_resources",
    srcs = ["//cyber:runtime_resources"],
    prefix = "/opt/wheelos_core/resources",
    strip_prefix = strip_prefix.from_root(),
)

pkg_deb(
    name = "wheelos_core",
    architecture = "amd64",
    built_using = "unzip",
    data = ":wheelos_cyber_runtime",
    depends = [
        "unzip",
    ],
    description = "Wheelos Core runtime bundle",
    homepage = "https://github.com/wheelos/core",
    maintainer = "daohu527@gmail.com",
    package = "wheelos_core",
    version = "1.0.6",
    visibility = ["//visibility:public"],
)

pkg_tar(
    name = "wheelos_cyber_runtime",
    srcs = [
        ":wheelos_cyber_runtime_launcher",
        ":wheelos_cyber_runtime_launcher_source",
        ":wheelos_cyber_runtime_library",
        ":wheelos_cyber_runtime_native_tools",
        ":wheelos_cyber_runtime_resources",
        ":wheelos_cyber_runtime_setup",
    ],
    visibility = ["//visibility:public"],
)

pkg_tar(
    name = "wheelos_cyber_sdk",
    visibility = ["//visibility:public"],
    deps = [
        "wheelos_cyber_api_manifest",
        "wheelos_cyber_conf",
        "wheelos_cyber_core",
        "wheelos_cyber_header",
    ],
)

pkg_tar(
    name = "wheelos_cyber_core",
    srcs = [
        "//cyber:cyber_core",
    ],
    mode = "0755",
    package_dir = "/usr/local/lib",
)

pkg_tar(
    name = "wheelos_cyber_header",
    srcs = [":wheelos_cyber_header_files"],
    mode = "0755",
)

pkg_tar(
    name = "wheelos_cyber_conf",
    srcs = [
        "//cyber:cyber_conf",
    ],
    mode = "0644",
    package_dir = "/etc/cyber",
)
