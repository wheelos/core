def core_deps():
    # ref https://github.com/grpc/grpc/blob/master/bazel/grpc_deps.bzl
    native.new_local_repository(
        name = "uuid",
        build_file = "@core//bazel/third_party:uuid.BUILD",
        path = "/usr/include",
    )

    native.new_local_repository(
        name = "fastcdr",
        build_file = "@core//bazel/third_party:fastcdr.BUILD",
        path = "/usr/local/fast-rtps/include",
    )

    native.new_local_repository(
        name = "fastrtps",
        build_file = "@core//bazel/third_party:fastrtps.BUILD",
        path = "/usr/local/fast-rtps/include",
    )

    native.new_local_repository(
        name = "ncurses5",
        build_file = "@core//bazel/third_party:ncurses5.BUILD",
        path = "/usr/include",
    )
