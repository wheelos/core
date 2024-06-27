def core_deps():
    # ref https://github.com/grpc/grpc/blob/master/bazel/grpc_deps.bzl
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
