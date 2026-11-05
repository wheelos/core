load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")
load("@rules_pkg//pkg:install.bzl", "pkg_install")
load("@rules_pkg//pkg:tar.bzl", "pkg_tar")
load("@rules_python//python:defs.bzl", "py_binary", "py_library", "py_test")
load(":product.bzl", "runtime_product")
load(":runtime.bzl", "runtime_layout", "runtime_package", "runtime_run")
load(":runtime_test.bzl", "runtime_analysis_tests")

package(default_visibility = ["//visibility:public"])

exports_files([
    "materialize_runtime.py",
    "runtime_entry.py",
    "runtime.bzl",
    "product.bzl",
    "empty",
])

py_library(
    name = "runtime_support",
    srcs = [
        "materialize_runtime.py",
        "runtime_entry.py",
    ],
)

py_binary(
    name = "materialize_runtime",
    srcs = ["materialize_runtime.py"],
    data = ["runtime_entry.py"],
)

py_library(
    name = "python_runtime",
    deps = [
        "//cyber/proto:record_py_pb2",
        "//cyber/python/cyber_py3:cyber",
        "//cyber/python/cyber_py3:cyber_time",
        "//cyber/python/cyber_py3:cyber_timer",
        "//cyber/python/cyber_py3:parameter",
        "//cyber/python/cyber_py3:record",
    ],
)

py_test(
    name = "materialize_runtime_test",
    srcs = ["materialize_runtime_test.py"],
    deps = [":runtime_support"],
)

runtime_analysis_tests(name = "analysis_tests")

runtime_product(
    name = "product_fixture",
    testonly = True,
    default_dag = "dag/example/example.dag",
    modules = [":example_module"],
    package_file_name = "fixture-product.tar.gz",
    tags = ["manual"],
    visibility = ["//visibility:private"],
)

py_binary(
    name = "fixture_diagnose",
    testonly = True,
    srcs = ["tests/diagnose.py"],
    main = "tests/diagnose.py",
)

cc_library(
    name = "fixture_dependency",
    testonly = True,
    srcs = ["tests/fixture_dependency.cc"],
    linkstatic = False,
)

cc_binary(
    name = "libfixture_component.so",
    testonly = True,
    srcs = ["tests/fixture_component.cc"],
    linkshared = True,
    linkstatic = False,
    deps = [":fixture_dependency"],
)

genrule(
    name = "fake_mainboard",
    testonly = True,
    srcs = ["tests/fake_mainboard.sh"],
    outs = ["fixture_mainboard"],
    cmd = "cp $(execpath tests/fake_mainboard.sh) $@ && chmod +x $@",
    executable = True,
)

runtime_layout(
    name = "fixture_layout",
    testonly = True,
    cyber_conf = "tests/example.conf",
    default_dag = "dag/example/example.dag",
    modules = [":example_module"],
    python_runtime = [],
    tools = {":fake_mainboard": "bin/mainboard"},
)

runtime_run(
    name = "fixture_run",
    testonly = True,
    runtime = ":fixture_layout",
)

runtime_package(
    name = "fixture_package_files",
    testonly = True,
    runtime = ":fixture_layout",
)

pkg_tar(
    name = "fixture_package",
    testonly = True,
    srcs = [":fixture_package_files"],
)

pkg_install(
    name = "fixture_install",
    testonly = True,
    srcs = [":fixture_package_files"],
)

py_test(
    name = "runtime_library_test",
    srcs = ["runtime_library_test.py"],
    data = [":fixture_layout"],
    deps = [":runtime_support"],
)
