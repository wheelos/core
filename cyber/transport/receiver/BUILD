load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//:internal"])

cc_library(
    name = "receiver",
    deps = [
        ":hybrid_receiver",
        ":iceoryx_receiver",
        ":intra_receiver",
        ":rtps_receiver",
        ":shm_receiver",
    ],
)

cc_library(
    name = "receiver_interface",
    hdrs = ["receiver.h"],
    deps = [
        "//cyber/transport/common:endpoint",
        "//cyber/transport/message:history",
        "//cyber/transport/message:message_info",
    ],
)

cc_library(
    name = "hybrid_receiver",
    hdrs = ["hybrid_receiver.h"],
    deps = [
        ":receiver_interface",
    ],
)

cc_library(
    name = "intra_receiver",
    hdrs = ["intra_receiver.h"],
    deps = [
        ":receiver_interface",
    ],
)

cc_library(
    name = "iceoryx_receiver",
    hdrs = [
        "iceoryx_event_bridge.h",
        "iceoryx_receiver.h",
    ],
    deps = [
        ":receiver_interface",
        "//cyber/task",
        "//cyber/transport:iceoryx_chunk",
        "//cyber/transport:iceoryx_runtime",
        "//cyber/transport/dispatcher:shm_dispatcher",
        "//cyber/transport/shm:profile",
        "@iceoryx//:iceoryx_posh",
    ],
)

cc_library(
    name = "rtps_receiver",
    hdrs = ["rtps_receiver.h"],
    deps = [
        ":receiver_interface",
    ],
)

cc_library(
    name = "shm_receiver",
    hdrs = ["shm_receiver.h"],
    deps = [
        ":receiver_interface",
        "//cyber/transport/shm:readable_info",
    ],
)
