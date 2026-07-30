# wheelos_core

`wheelos_core` is a Cyber RT middleware foundation for autonomous-driving
applications. It provides three separate, supported interfaces:

| Need | Interface |
| --- | --- |
| Build C++ or Bazel Python applications | Bzlmod source SDK |
| Run native Cyber processes and tools | `/opt/wheelos_core` runtime bundle |
| Install Python bindings | Version-matched `pycyber` wheel |

Do not use copied headers, Bazel output paths, runfiles directories, or a
hand-built `PYTHONPATH` as an SDK interface.

## Build the repository

```bash
sudo bash scripts/deploy/build.sh
bash scripts/build.sh
```

The release runtime package is built with:

```bash
bazel build //:wheelos_core
sudo apt install ./bazel-bin/wheelos_core_1.0.0_all.deb
```

The installed bundle contains `mainboard`, `cyber_launch`, `cyber_monitor`,
`cyber_recorder`, runtime libraries, and configuration resources. For an
interactive shell:

```bash
source /opt/wheelos_core/setup.bash
cyber_launch --help
```

The Debian artifact is a runtime bundle, not an installed C++ SDK. Use Bzlmod
for C++ development.

## Consume from Bazel

Reusable BUILD files use the public module name and C++ facade:

```starlark
# MODULE.bazel
bazel_dep(name = "wheelos_core", version = "1.0.0")
```

```python
cc_binary(
    name = "my_node",
    srcs = ["my_node.cc"],
    deps = ["@wheelos_core//cyber:cyber"],
)
```

Downstream workspaces must enable C++17:

```text
build --cxxopt=-std=c++17
build --host_cxxopt=-std=c++17
```

The public registry release is not yet published. During local development,
resolve the source explicitly:

```bash
bazel build --override_module=wheelos_core=/path/to/core //:my_node
bazel run --override_module=wheelos_core=/path/to/core \
  @wheelos_core//cyber/tools/cyber_launch:cyber_launch -- --help
```

Use `@wheelos_core//cyber:runtime_tools` for the supported Bazel development
tool set. `mainboard` is a process launcher; applications should link
`//cyber:cyber`, not `mainboard`.

## Python and releases

Build the runtime package and matching Python artifacts together:

```bash
bash scripts/release/build_release_artifacts.sh
python3 -m pip install artifacts/release/pycyber/pycyber-*.whl
```

Release tags use `wheelos_core-v<module-version>`, for example
`wheelos_core-v1.0.0`. Untagged builds produce a deterministic prerelease.

Before release, run:

```bash
bash scripts/release/check_bzlmod_lockfile.sh --check
bash scripts/release/validate_downstream_bazel_sdk.sh
bash scripts/release/validate_runtime_bundle.sh
```

See [the installation and usage guide](docs/doxy-docs/source/Wheelos_Core_Installation_and_Usage.md)
for deployment, Python, service-manager, and publication details.
