"""One product paradigm for source runs, installation, and archives."""

load("@rules_pkg//pkg:install.bzl", "pkg_install")
load("@rules_pkg//pkg:tar.bzl", "pkg_tar")
load(":runtime.bzl", "runtime_layout", "runtime_package", "runtime_run")

def runtime_product(
        name,
        modules,
        default_dag = "",
        default_launch = "",
        files = {},
        cyber_conf = None,
        required_assets = {},
        required_configs = [],
        package_file_name = None,
        visibility = None,
        testonly = False,
        tags = []):
    """Declare one source-built runtime and its matching delivery targets.

    Args:
        name: Runtime tree target; sibling names append delivery/action suffixes.
        modules: Explicit RuntimeModuleInfo contributions.
        files: Product-owned label-to-destination file mappings.
        cyber_conf: Optional replacement for Core's shared Cyber configuration.
        default_dag: Runtime-relative default DAG, exclusive with default_launch.
        default_launch: Runtime-relative default launch, exclusive with default_dag.
        required_assets: Asset-root-relative keys mapped to reviewed SHA-256 digests.
        required_configs: Keys required in an explicit external config root.
        package_file_name: Optional archive filename; defaults to name + ".tar.gz".
        visibility: Visibility applied consistently to every generated target.
        testonly: Whether the product is restricted to tests.
        tags: Tags applied to every generated target.
    """
    common = {"testonly": testonly, "tags": tags}
    if visibility != None:
        common["visibility"] = visibility
    configuration = {}
    if cyber_conf != None:
        configuration["cyber_conf"] = cyber_conf
    runtime_layout(
        name = name,
        modules = modules,
        files = files,
        default_dag = default_dag,
        default_launch = default_launch,
        required_assets = required_assets,
        required_configs = required_configs,
        **dict(common, **configuration)
    )
    runtime_run(
        name = name + "_run",
        runtime = ":" + name,
        **common
    )
    runtime_package(
        name = name + "_files",
        runtime = ":" + name,
        **common
    )
    pkg_tar(
        name = name + "_package",
        srcs = [":" + name + "_files"],
        extension = "tar.gz",
        package_file_name = package_file_name or name + ".tar.gz",
        **common
    )
    pkg_install(
        name = name + "_install",
        srcs = [":" + name + "_files"],
        **common
    )
