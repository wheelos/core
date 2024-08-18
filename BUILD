load("@rules_pkg//:pkg.bzl", "pkg_deb", "pkg_tar")

pkg_deb(
    name = "wheelos_core",
    built_using = "unzip",
    data = ":wheelos_cyber",
    depends = [
        "unzip",
    ],
    description = "Wheelos Core",
    homepage = "https://github.com/wheelos/core",
    maintainer = "daohu527@gmail.com",
    package = "wheelos_core",
    version = "0.2.0",
)

pkg_tar(
    name = "wheelos_cyber",
    package_dir = "/usr/local/bin",
    srcs = [
        "//cyber",
    ],
    deps = [
        ":wheelos_cyber_conf",
    ],
    mode = "0755",
)

pkg_tar(
    name = "wheelos_cyber_conf",
    package_dir = "/etc/cyber/conf",
    srcs = glob([
        "cyber/conf/*.conf",
    ]),
    mode = "0644",
)

pkg_tar(
    name = "wheelos_smart_recorder",
    package_dir = "/usr/local/bin",
    srcs = [
        "//tools/smart_recorder",
    ],
    deps = [
        ":wheelos_smart_recorder_conf",
    ],
    mode = "0755",
)

pkg_tar(
    name = "wheelos_smart_recorder_conf",
    package_dir = "/etc/tools/smart_recorder/conf",
    srcs = glob([
        "tools/smart_recorder/conf/*.conf",
    ]),
    mode = "0644",
)
