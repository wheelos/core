load("@rules_cc//cc:defs.bzl", "cc_binary")
load("@rules_python//python:defs.bzl", "py_test")

package(default_visibility = ["//visibility:public"])

cc_binary(
    name = "libinstalled_e2e_component.so",
    srcs = ["installed_e2e_component.cc"],
    linkshared = True,
    linkstatic = False,
    deps = ["//cyber"],
)

filegroup(
    name = "fixtures",
    srcs = [":libinstalled_e2e_component.so"],
)

filegroup(
    name = "installed_runtime_cases",
    srcs = [
        "installed_runtime_test.py",
        "runtime_worker.py",
    ],
)

py_test(
    name = "harness_test",
    size = "small",
    srcs = [
        "installed_runtime_harness_test.py",
        "installed_runtime_test.py",
    ],
    main = "installed_runtime_harness_test.py",
    deps = ["//scripts/testing:artifacts"],
)
