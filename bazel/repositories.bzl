load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
#load("@bazel_tools//tools/build_defs/repo:local.bzl", "new_local_repository")

def core_repositories():
    http_archive(
        name = "com_github_grpc_grpc",
        sha256 = "419dba362eaf8f1d36849ceee17c3e2ff8ff12ac666b42d3ff02a164ebe090e9",
        strip_prefix = "grpc-1.30.0",
        urls = [
            "https://github.com/grpc/grpc/archive/v1.30.0.tar.gz",
        ],
    )    

    http_archive(
        name = "com_github_google_glog",
        sha256 = "00e4a87e87b7e7612f519a41e491f16623b12423620006f59f5688bfd8d13b08",
        strip_prefix = "glog-0.7.1",
        urls = [
            "https://github.com/google/glog/archive/refs/tags/v0.7.1.tar.gz",
        ],
    )        

    http_archive(
        name = "com_github_gflags_gflags",
        sha256 = "34af2f15cf7367513b352bdcd2493ab14ce43692d2dcd9dfc499492966c64dcf",
        strip_prefix = "gflags-2.2.2",
        urls = ["https://github.com/gflags/gflags/archive/v2.2.2.tar.gz"],
    )

    http_archive(
        name = "com_google_absl",
        urls = ["https://github.com/abseil/abseil-cpp/archive/98eb410c93ad059f9bba1bf43f5bb916fc92a5ea.zip"],
        strip_prefix = "abseil-cpp-98eb410c93ad059f9bba1bf43f5bb916fc92a5ea",
    )

    http_archive(
        name = "com_google_protobuf",
        sha256 = "6fbe2e6f703bcd3a246529c2cab586ca12a98c4e641f5f71d51fde09eb48e9e7",
        strip_prefix = "protobuf-27.1",
        urls = [
            "https://github.com/protocolbuffers/protobuf/archive/refs/tags/v27.1.tar.gz",
        ],
    )

#    new_local_repository(
#        name = "uuid",
#        build_file = "@cyber//bazel/third_party:uuid.BUILD",
#        path = "/usr/include",
#    )
