load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

def core_deps():
# ref https://github.com/grpc/grpc/blob/master/bazel/grpc_deps.bzl

#    http_archive(
#        name = "zlib",
#        build_file = "@com_google_protobuf//:third_party/zlib.BUILD",
#        sha256 = "629380c90a77b964d896ed37163f5c3a34f6e6d897311f1df2a7016355c45eff",
#        strip_prefix = "zlib-1.2.11",
#        urls = ["https://github.com/madler/zlib/archive/v1.2.11.tar.gz"],
#    )

#    http_archive(
#        name = "com_google_absl",
#        urls = ["https://github.com/abseil/abseil-cpp/archive/refs/tags/20240116.2.tar.gz"],
#        strip_prefix = "abseil-cpp-20240116.2",
#    )

    http_archive(
        name = "com_github_google_glog",
        sha256 = "00e4a87e87b7e7612f519a41e491f16623b12423620006f59f5688bfd8d13b08",
        strip_prefix = "glog-0.7.1",
        urls = [
            "https://github.com/google/glog/archive/refs/tags/v0.7.1.tar.gz",
        ],
    )

    http_archive(
        name = "com_github_google_glog",
        sha256 = "f28359aeba12f30d73d9e4711ef356dc842886968112162bc73002645139c39c",
        strip_prefix = "glog-0.4.0",
        urls = [
            "https://apollo-system.cdn.bcebos.com/archive/6.0/v0.4.0.tar.gz",
            "https://github.com/google/glog/archive/v0.4.0.tar.gz",
        ],
    )    

    http_archive(
        name = "com_github_gflags_gflags",
        sha256 = "34af2f15cf7367513b352bdcd2493ab14ce43692d2dcd9dfc499492966c64dcf",
        strip_prefix = "gflags-2.2.2",
        urls = ["https://github.com/gflags/gflags/archive/v2.2.2.tar.gz"],
    )

    http_archive(
        name = "com_google_protobuf",
        sha256 = "d0f5f605d0d656007ce6c8b5a82df3037e1d8fe8b121ed42e536f569dec16113",
        strip_prefix = "protobuf-3.14.0",
        urls = [
            "https://apollo-system.cdn.bcebos.com/archive/6.0/v3.14.0.tar.gz",
            "https://github.com/protocolbuffers/protobuf/archive/v3.14.0.tar.gz",
        ],
    )

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
