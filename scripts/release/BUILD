load("@rules_python//python:defs.bzl", "py_binary", "py_library", "py_test")

package(default_visibility = ["//visibility:public"])

py_library(
    name = "zero_loss_capacity_lib",
    srcs = ["zero_loss_capacity.py"],
)

py_binary(
    name = "run_zero_loss_capacity",
    srcs = ["run_zero_loss_capacity.py"],
    deps = [":zero_loss_capacity_lib"],
)

py_test(
    name = "zero_loss_capacity_test",
    size = "small",
    srcs = ["zero_loss_capacity_test.py"],
    deps = [":zero_loss_capacity_lib"],
)

py_test(
    name = "release_scripts_test",
    size = "small",
    srcs = ["release_scripts_test.py"],
    data = [
        "build_release_artifacts.sh",
        "run_release_acceptance.sh",
        "ubuntu2204_baseline.sh",
        "validate_runtime_bundle.sh",
    ],
)
