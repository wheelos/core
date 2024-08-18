load("@bazel_tools//tools/build_defs/pkg:pkg.bzl", "pkg_tar", "pkg_deb")

pkg_deb(
    name = "wheelos_core",
    built_using = "unzip",
    data = [
        ":wheelos_cyber",
        ":wheelos_smart_recorder",
    ],
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
    package_dir = "",
    srcs = [
        "//cyber",
    ],
    deps = [
        ":wheelos_cyber_conf",
    ]
    mode = "0755",
)

pkg_tar(
    name = "wheelos_cyber_conf",
    package_dir = "",
    srcs = glob([
        "cyber/conf/*.conf",
    ]),
    mode = "0755",
)

filegroup(
    name = "cyber_header",
    srcs = [
        "cyber/cyber.h",
        "cyber/init.h",
        "cyber/state.h",
        "cyber/base/atomic_hash_map.h",
        "cyber/base/atomic_rw_lock.h",
        "cyber/base/bounded_queue.h",
        "cyber/base/concurrent_object_pool.h",
        "cyber/base/for_each.h",
        "cyber/base/macros.h",
        "cyber/base/object_pool.h",
        "cyber/base/reentrant_rw_lock.h",
        "cyber/base/rw_lock_guard.h",
        "cyber/base/signal.h",
        "cyber/base/thread_pool.h",
        "cyber/base/thread_safe_queue.h",
        "cyber/base/unbounded_queue.h",
        "cyber/base/wait_strategy.h",
        "cyber/blocker/blocker.h",
        "cyber/blocker/intra_reader.h",
        "cyber/blocker/intra_writer.h",
        "cyber/common/environment.h",
        "cyber/common/file.h",
        "cyber/common/global_data.h",
        "cyber/common/log.h",
        "cyber/common/time_conversion.h",
        "cyber/common/util.h",
        "cyber/component/component.h",
        "cyber/component/timer_component.h",
        "cyber/croutine/croutine.h",
        "cyber/croutine/routine_factory.h",
        "cyber/data/data_visitor.h",
        "cyber/message/message_traits.h",
        "cyber/node/node_channel_impl.h",
        "cyber/node/node_service_impl.h",
        "cyber/node/node.h",
        "cyber/node/reader_base.h",
        "cyber/node/reader.h",
        "cyber/node/writer_base.h",
        "cyber/node/writer.h",
        "cyber/scheduler/scheduler_factory.h",
        "cyber/service/client.h",
        "cyber/service/service.h",
        "cyber/service_discovery/topology_manager.h",
        "cyber/parameter/parameter_server.h",
        "cyber/parameter/parameter_client.h",
        "cyber/task/task.h",
        "cyber/time/clock.h",
        "cyber/time/duration.h",
        "cyber/time/rate.h",
        "cyber/time/time.h",
        "cyber/timer/timer.h",
        "cyber/transport/transport.h",
        "cyber/record/record_reader.h",
        "cyber/record/record_viewer.h",
        "cyber/record/record_writer.h"
    ]
)

pkg_tar(
    name = "wheelos_smart_recorder",
    package_dir = "",
    srcs = [
        "//tools/smart_recorder",
    ],
    deps = [
        ":wheelos_smart_recorder_conf",
    ]
    mode = "0755",
)

pkg_tar(
    name = "wheelos_smart_recorder_conf",
    package_dir = "",
    srcs = glob([
        "tools/smart_recorder/conf/*.conf",
    ]),
    mode = "0755",
)
