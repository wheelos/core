load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "cyber",
    linkstatic = False,
    deps = [
        ":cyber_core",
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
    deps = [
        "//cyber/common",
    ],
)

cc_library(
    name = "init",
    srcs = ["init.cc"],
    hdrs = ["init.h"],
    deps = [
        "//cyber:binary",
        "//cyber:state",
        "//cyber/common:file",
        "//cyber/logger:async_logger",
        "//cyber/node",
        "//cyber/proto:clock_cc_proto",
        "//cyber/sysmo",
        "//cyber/time:clock",
        "//cyber/timer:timing_wheel",
    ],
)

cc_library(
    name = "cyber_core",
    srcs = ["cyber.cc"],
    hdrs = ["cyber.h"],
    linkopts = ["-lrt"],
    deps = [
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
        "@wheelos_fastdds//:fastdds",
    ],
)

filegroup(
    name = "cyber_conf",
    srcs = glob([
        "conf/*.conf",
    ]),
)

filegroup(
    name = "cyber_header",
    srcs = [
        "cyber.h",
        "init.h",
        "state.h",
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
