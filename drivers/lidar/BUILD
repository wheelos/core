load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")


package(default_visibility = ["//visibility:public"])


filegroup(
    name = "runtime_data",
    srcs = glob([
        "conf/*.txt",
        "conf/*.conf",
        "dag/*.dag",
        "launch/*.launch",
    ]),
)

cc_library(
    name = "lidar_driver_component_lib",
    srcs = ["lidar_driver_component.cc"],
    hdrs = ["lidar_driver_component.h"],
    deps = [
        "//cyber",
        "//modules/drivers/lidar/common/driver_factory:lidar_driver_factory",
        "//modules/drivers/lidar/proto:config",
    ],
)

cc_binary(
    name = "liblidar_driver_component.so",
    linkshared = True,
    linkstatic = False,
    deps = [":lidar_driver_component_lib"],
)


