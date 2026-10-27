load("@pybind11_bazel//:build_defs.bzl", "pybind_extension")

package(default_visibility = ["//:internal"])

pybind_extension(
    name = "_cyber_wrapper",
    srcs = [
        "binding_common.cc",
        "binding_common.h",
        "binding_registrars.h",
        "callback_lifecycle.cc",
        "callback_lifecycle.h",
        "module_state.cc",
        "module_state.h",
        "node_bindings.cc",
        "node_bindings.h",
        "parameter_bindings.cc",
        "pybind_module.cc",
        "record_bindings.cc",
        "runtime_bindings.cc",
        "time_bindings.cc",
        "timer_bindings.cc",
        "topology_bindings.cc",
    ],
    deps = [
        "//cyber:cyber_core",
        "//cyber:python_lifecycle",
        "//cyber/message:protobuf_factory",
        "//cyber/message:py_message",
        "//cyber/message:raw_message",
        "//cyber/parameter",
        "//cyber/parameter:parameter_client",
        "//cyber/parameter:parameter_server",
        "//cyber/record",
        "@com_google_protobuf//:protobuf",
        "@fastdds",
    ],
)
