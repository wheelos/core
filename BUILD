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
    package_dir = "/usr/local/include",
    srcs = [
        "//cyber:cyber_header",
    ],
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
