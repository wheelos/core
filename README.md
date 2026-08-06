# wheelos_core

`wheelos_core` is a C++17, Bazel-first Cyber RT middleware foundation for
high-throughput autonomous systems. The repository contains the runtime,
public C++ APIs, Python bindings, tools, examples, and release scripts.

## Choose an integration surface

| Goal | Supported interface |
| --- | --- |
| Build a C++ or Bazel Python application | Bzlmod source dependency |
| Run native processes and tools | `/opt/wheelos_core` runtime bundle |
| Use Python from a normal Python environment | Version-matched `pycyber` wheel |

Do not treat copied headers, `bazel-bin`, runfiles, or a hand-built
`PYTHONPATH` as a supported SDK.

## Build and test

Install the build environment and run the repository build:

```bash
sudo bash scripts/deploy/build.sh
bash scripts/build.sh
```

Useful CI-aligned checks:

```bash
bazel build //cyber
bazel test //cyber/message/...
bash scripts/release/check_bzlmod_lockfile.sh --check
```

See the [documentation index](docs/doxy-docs/source/index.md) for the
installation, component, API, scheduling, and release guides.

## Build a runtime package

```bash
bazel build //:wheelos_core
sudo apt install ./bazel-bin/wheelos_core_1.0.0_all.deb
source /opt/wheelos_core/setup.bash
cyber_launch --help
```

The Debian package is a runtime bundle. It provides `mainboard`,
`cyber_launch`, `cyber_monitor`, `cyber_recorder`, libraries, and resources;
use Bzlmod for C++ development headers and dependencies.

## Consume from Bazel

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

Enable C++17 in the downstream workspace:

```text
build --cxxopt=-std=c++17
build --host_cxxopt=-std=c++17
```

For a local checkout, use an explicit module override:

```bash
bazel build --override_module=wheelos_core=/path/to/core //:my_node
```

## Python and releases

Build matching runtime and Python artifacts together:

```bash
bash scripts/release/build_release_artifacts.sh
python3 -m pip install artifacts/release/pycyber/pycyber-*.whl
```

Release tags use `wheelos_core-v<module-version>`, for example
`wheelos_core-v1.0.0`. Run the release validation scripts before publishing.
