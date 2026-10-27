load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//:internal"])

cc_library(
    name = "cyber",
    implementation_deps = [":cyber_core"],
    linkstatic = False,
    visibility = ["//visibility:public"],
    deps = [
        ":api",
        "//cyber/common:file",
        "//cyber/common:macros",
        "//cyber/common:resource_manager",
        "//cyber/component:timer_component",
        "//cyber/message:raw_message",
        "//cyber/parameter:parameter_client",
        "//cyber/parameter:parameter_server",
        "//cyber/record",
        "//cyber/time:clock",
        "//cyber/time:duration",
        "//cyber/time:rate",
    ],
)

cc_library(
    name = "binary",
    srcs = ["binary.cc"],
    hdrs = ["binary.h"],
)

cc_library(
    name = "state",
    srcs = ["state.cc"],
    hdrs = ["state.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//cyber/common:log",
    ],
)

cc_library(
    name = "init",
    srcs = ["init.cc"],
    hdrs = ["init.h"],
    implementation_deps = [
        "//cyber:binary",
        "//cyber:python_lifecycle",
        "//cyber/common:file",
        "//cyber/common:global_data",
        "//cyber/data:data_dispatcher",
        "//cyber/logger:async_logger",
        "//cyber/metrics",
        "//cyber/node",
        "//cyber/proto:clock_cc_proto",
        "//cyber/scheduler",
        "//cyber/service_discovery:topology_manager",
        "//cyber/sysmo",
        "//cyber/task",
        "//cyber/time:clock",
        "//cyber/timer:timing_wheel",
        "//cyber/transport",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//cyber:state",
        "//cyber/common:log",
    ],
)

cc_library(
    name = "api",
    hdrs = ["cyber.h"],
    deps = [
        ":init",
        "//cyber/common:log",
        "//cyber/component",
        "//cyber/node",
        "//cyber/task",
        "//cyber/time",
        "//cyber/timer",
    ],
)

cc_library(
    name = "python_lifecycle",
    hdrs = ["internal/python_lifecycle.h"],
)

cc_library(
    name = "cyber_core",
    srcs = ["cyber.cc"],
    linkopts = ["-lrt"],
    deps = [
        ":api",
        "//cyber:binary",
        "//cyber:init",
        "//cyber:state",
        "//cyber/base",
        "//cyber/blocker:blocker_manager",
        "//cyber/class_loader",
        "//cyber/class_loader:class_loader_manager",
        "//cyber/common",
        "//cyber/component",
        "//cyber/component:timer_component",
        "//cyber/croutine",
        "//cyber/data",
        "//cyber/event:perf_event_cache",
        "//cyber/io",
        "//cyber/logger",
        "//cyber/logger:async_logger",
        "//cyber/message:message_traits",
        "//cyber/message:protobuf_traits",
        "//cyber/message:py_message_traits",
        "//cyber/message:raw_message_traits",
        "//cyber/node",
        "//cyber/parameter:parameter_client",
        "//cyber/parameter:parameter_server",
        "//cyber/proto:run_mode_conf_cc_proto",
        "//cyber/record",
        "//cyber/scheduler",
        "//cyber/scheduler:scheduler_factory",
        "//cyber/service",
        "//cyber/service:client",
        "//cyber/service_discovery:topology_manager",
        "//cyber/sysmo",
        "//cyber/task",
        "//cyber/time",
        "//cyber/time:clock",
        "//cyber/time:duration",
        "//cyber/time:rate",
        "//cyber/timer",
        "//cyber/transport",
        "//cyber/transport/rtps:participant",
        "//cyber/transport/rtps:sub_listener",
        "@com_github_google_glog//:glog",
        "@com_google_protobuf//:protobuf",
        "@fastdds",
    ],
)

filegroup(
    name = "runtime_tools",
    srcs = [
        "//cyber/mainboard",
        "//cyber/tools/cyber_channel",
        "//cyber/tools/cyber_launch",
        "//cyber/tools/cyber_monitor",
        "//cyber/tools/cyber_node",
        "//cyber/tools/cyber_recorder",
        "//cyber/tools/cyber_service",
    ],
    visibility = ["//visibility:public"],
)

filegroup(
    name = "runtime_native_tools",
    srcs = [
        "//cyber/mainboard",
        "//cyber/tools/cyber_monitor",
        "//cyber/tools/cyber_recorder",
    ],
)

filegroup(
    name = "runtime_resources",
    srcs = glob(
        [
            "conf/**",
        ],
        allow_empty = True,
    ) + ["//cyber/tools:cyber_tools_auto_complete.bash"],
)

filegroup(
    name = "cyber_conf",
    srcs = glob([
        "conf/*.conf",
    ]),
    visibility = ["//visibility:public"],
)

filegroup(
    name = "cyber_header",
    srcs = [
        "binary.h",
        "cyber.h",
        "init.h",
        "state.h",
    ],
)
