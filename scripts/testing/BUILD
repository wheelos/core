load("@rules_python//python:defs.bzl", "py_library", "py_test")

py_library(
    name = "artifacts",
    srcs = ["artifacts.py"],
    imports = ["."],
    visibility = ["//tests/e2e:__pkg__"],
)

py_test(
    name = "testing_tools_test",
    size = "small",
    srcs = [
        "check_test_layers.py",
        "container_test.py",
        "qualify_release.py",
        "testing_tools_test.py",
    ],
    main = "testing_tools_test.py",
    deps = [":artifacts"],
)
