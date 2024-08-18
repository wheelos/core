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
        ":cyber_header",
        "//cyber:cyber_core"
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

filegroup(
    name = "cyber_header",
    srcs = [
        "//cyber:cyber_internal_header",
        "//cyber/base:base_header",
        "//cyber/blocker:blocker_header",
        "//cyber/common:common_header",
        "//cyber/component:component_header",
        "//cyber/croutine:croutine_header",
        "//cyber/data:data_header",
        "//cyber/message:message_header",
        "//cyber/node:node_header",
        "//cyber/parameter:parameter_header",
        "//cyber/record:record_header",
        "//cyber/scheduler:scheduler_header",
        "//cyber/service:service_header",
        "//cyber/service_discovery:service_discovery_header",
        "//cyber/task:task_header",
        "//cyber/time:time_header",
        "//cyber/timer:timer_header",
        "//cyber/transport:transport_header",
    ]
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
