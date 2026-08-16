load("@rules_pkg//:pkg.bzl", "pkg_deb", "pkg_tar")
load("@rules_pkg//pkg:mappings.bzl", "pkg_attributes", "pkg_files", "strip_prefix")
load("//tools/sdk:cc_headers.bzl", "cc_sdk_headers")

cc_sdk_headers(
    name = "wheelos_cyber_headers",
    target = "//cyber:cyber",
)

pkg_files(
    name = "wheelos_cyber_header_files",
    srcs = [":wheelos_cyber_headers"],
    prefix = "/usr/local/include",
    strip_prefix = strip_prefix.from_root(),
)

pkg_files(
    name = "wheelos_cyber_runtime_library",
    srcs = ["//cyber:cyber_core"],
    prefix = "/opt/wheelos_core/lib",
    strip_prefix = strip_prefix.files_only(),
    attributes = pkg_attributes(mode = "0755"),
)

pkg_files(
    name = "wheelos_cyber_runtime_native_tools",
    srcs = ["//cyber:runtime_native_tools"],
    prefix = "/opt/wheelos_core/bin",
    strip_prefix = strip_prefix.files_only(),
    attributes = pkg_attributes(mode = "0755"),
)

pkg_files(
    name = "wheelos_cyber_runtime_launcher",
    srcs = ["scripts/runtime/cyber_launch"],
    prefix = "/opt/wheelos_core/bin",
    strip_prefix = strip_prefix.files_only(),
    attributes = pkg_attributes(mode = "0755"),
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
    prefix = "/opt/wheelos_core",
    strip_prefix = strip_prefix.files_only(),
    attributes = pkg_attributes(mode = "0755"),
)

pkg_files(
    name = "wheelos_cyber_runtime_resources",
    srcs = ["//cyber:runtime_resources"],
    prefix = "/opt/wheelos_core/resources",
    strip_prefix = strip_prefix.from_root(),
)

pkg_deb(
    name = "wheelos_core",
    built_using = "unzip",
    data = ":wheelos_cyber_runtime",
    depends = [
        "unzip",
    ],
    description = "Wheelos Core runtime bundle",
    homepage = "https://github.com/wheelos/core",
    maintainer = "daohu527@gmail.com",
    package = "wheelos_core",
    version = "1.0.3",
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
)

pkg_tar(
    name = "wheelos_cyber_sdk",
    deps = [
        "wheelos_cyber_core",
        "wheelos_cyber_header",
        "wheelos_cyber_conf"
    ],
)

pkg_tar(
    name = "wheelos_cyber_core",
    package_dir = "/usr/local/lib",
    srcs = [
        "//cyber:cyber_core"
    ],
    mode = "0755",
)

pkg_tar(
    name = "wheelos_cyber_header",
    srcs = [":wheelos_cyber_header_files"],
    mode = "0755",
)

pkg_tar(
    name = "wheelos_cyber_conf",
    package_dir = "/etc/cyber",
    srcs = [
        "//cyber:cyber_conf",
    ],
    mode = "0644",
)
