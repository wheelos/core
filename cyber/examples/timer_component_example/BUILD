load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_binary(
    name = "libtimer_component_example.so",
    linkshared = True,
    linkstatic = False,
    deps = [":timer_component_example_lib"],
)

cc_library(
    name = "timer_component_example_lib",
    srcs = ["timer_component_example.cc"],
    hdrs = ["timer_component_example.h"],
    visibility = ["//visibility:private"],
    deps = [
        "//cyber",
        "//cyber/examples/proto:examples_cc_proto",
    ],
)

filegroup(
    name = "conf",
    srcs = [
        ":timer.dag",
        ":timer.launch",
    ],
)
