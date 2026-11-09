# Unified source runtime

`runtime.bzl` assembles a single module or any explicit module list with the
same development, installation and archive layout. Bzlmod resolves source
dependencies in the final product; assembly never starts devices or downloads
models.

For peer product roots, prefer the complete Core-owned macro:

```starlark
load("@core//runtime:product.bzl", "runtime_product")

runtime_product(
    name = "example",
    modules = [":example_module"],
    default_dag = "dag/example/example.dag",
)
```

This declares `:example` (runtime tree), `:example_run`, `:example_files`,
`:example_package` (tar.gz), and `:example_install` (`pkg_install`). Single
modules and arbitrary compositions use this same interface without depending
on Apollo-Lite. Product configuration includes `files`, optional `cyber_conf`,
mutually exclusive `default_dag`/`default_launch`, and the reviewed deployment
`required_assets`/`required_configs` described below.

For lower-level assembly or custom archive formats:

```starlark
load("@core//runtime:runtime.bzl", "runtime_layout", "runtime_module", "runtime_package", "runtime_run")

runtime_module(
    name = "module",
    module_name = "example",
    components = [":libexample.so"],
    files = {
        ":configuration": "conf/example/example.pb.txt",
        ":dag": "dag/example/example.dag",
    },
)
runtime_layout(
    name = "runtime",
    modules = [":module"],
    default_dag = "dag/example/example.dag",
)
runtime_run(name = "run", runtime = ":runtime")
runtime_package(name = "package_files", runtime = ":runtime")
```

Pass `package_files` to `@rules_pkg//pkg:tar.bzl`'s `pkg_tar` or
`@rules_pkg//pkg:install.bzl`'s `pkg_install`. Both consume the same materialized
tree, preserving executable permissions. `runtime_run` accepts `--dag KEY` or
`--launch KEY`; `default_dag` and `default_launch` are mutually exclusive.
Arguments following `--` are passed literally to mainboard.

## Contributions

`RuntimeModuleInfo` exports `name`, `(File, destination)` `entries`, and declared
transitive `libraries`. `runtime_module` accepts `components`, `executables`,
`files`, `resources`, and `dependency_metadata`. A `files` value is either one
exact destination (requiring one file) or a directory ending in `/` (file
basenames are appended). Directory artifacts preserve their subtree.

Module-owned paths are `conf/<module>/`, `dag/<module>/`, `launch/<module>/`,
`share/<module>/`, `lib/components/<module>/`, `bin/modules/<module>/` and
`assets/<module>/` or `assets/models/<module>/`. Module names must be unique.
The layout owns the common Cyber config and its single tool set. Product-owned
`files` in `runtime_product`/`runtime_layout` may explicitly override a module's
default `conf/<module>/...`, `dag/<module>/...`, or `launch/<module>/...` file.
Other conflicting contents, file/directory conflicts, missing inputs, and unsafe
paths fail. Identical files shared by declared dependencies are deduplicated.

`runtime_resources(files = [...], module_name = "...", namespace = "...")`
classifies source paths containing `conf`, `dag`, `launch`, `models`, `assets`,
`tools`, or `share`; paths after that directory are retained below the module
namespace. Models use `assets/models/<namespace>/`; `legacy_models = True`
(default) also installs explicit `models/<namespace>/` compatibility copies.
Set it false for new modules. Unclassified files are assets. `namespace` must
be the module name or begin with `<module>/`.

The layout installs declared default runfiles and `CcInfo` shared libraries,
including the core Python bindings and protobuf Python runfiles. `python/pycyber`
provides the wheel's public import spelling alongside source import paths.
External runfiles keys are rooted under their canonical repository directory.
Repository-mapping manifests from components sharing one runtime are merged;
ambiguous mappings fail materialization.
Rules_python venv interpreter links resolve only through declared runfiles;
module wrappers use that bundled interpreter and its relocated Python home.
No host-library scanning or separate binary ABI framework is involved.
`tools` and `python_runtime` may be replaced for hermetic assembly tests.
`required_runtime_input(message = "...")` is an analysis-failing placeholder
for configuration label flags that must be supplied.

## Relocation and deployment inputs

Source `bin/setup.bash` or invoke `bin/commands/{mainboard,cyber_launch,
cyber_recorder,cyber_monitor}`. Wrappers derive their installation root at each
invocation. `APOLLO_ROOT_DIR` and `CYBER_PATH` are that same root.
`LD_LIBRARY_PATH` contains only `<root>/lib` plus the explicit optional
`WHEELOS_RUNTIME_LIBRARY_PATH`, never the caller's Bazel cache. The default
asset root is `<root>/assets`; an explicit `WHEELOS_ASSET_ROOT` is preserved.
`WHEELOS_CONFIG_ROOT` is an explicit overlay, not a second software root.
Mainboard and launch run from the runtime root. Recorder and monitor retain
the caller's working directory; logging defaults to stderr unless configured.

`runtime_layout(required_assets = {"models/example/model": "<sha256>"},
required_configs = ["conf/example/reviewed.pb.txt"])` records reviewed
deployment requirements in `conf/runtime.json`. Before mainboard/launch starts,
required assets must exist inside the selected asset root and match SHA-256;
required config keys must exist inside an **explicit** `WHEELOS_CONFIG_ROOT`
(no fallback to shipped defaults). Traversal and escaping symlinks fail.
Recorder/monitor do not require these deployment inputs.

Focused checks:
`bazel test //runtime:materialize_runtime_test //runtime:analysis_tests //runtime:runtime_library_test`.
`fixture_package` and
`fixture_install` exercise delivery without native Cyber tools or devices.
