load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "base",
    testonly = True,
    hdrs = ["base.h"],
)

cc_library(
    name = "plugin1",
    testonly = True,
    srcs = ["plugin1.cc"],
    deps = [
        ":base",
        "//cyber/class_loader:class_loader_manager",
    ],
)

cc_library(
    name = "plugin2",
    testonly = True,
    srcs = ["plugin2.cc"],
    deps = [
        ":base",
        "//cyber/class_loader:class_loader_manager",
    ],
)

cc_binary(
    name = "plugin1_loader.so",
    testonly = True,
    linkshared = True,
    linkstatic = False,
    visibility = ["//visibility:public"],
    deps = [
        ":plugin1",
    ],
)

cc_binary(
    name = "plugin2_loader.so",
    testonly = True,
    linkshared = True,
    linkstatic = False,
    visibility = ["//visibility:public"],
    deps = [
        ":plugin2",
    ],
)
