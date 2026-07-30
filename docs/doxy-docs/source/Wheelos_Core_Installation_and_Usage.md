# Installing and using wheelos_core

`wheelos_core` has three deliberately separate consumption surfaces:

| Need | Supported interface | Do not use |
| --- | --- | --- |
| Develop a C++ or Bazel Python application | Bzlmod source SDK | Copied headers, `bazel-bin` paths, or `PYTHONPATH` |
| Run native Cyber processes and tools | Versioned `/opt/wheelos_core` runtime bundle | A source checkout's environment script |
| Install Python bindings | Version-matched `pycyber` wheel | Python modules copied from a Bazel runfiles tree |

This separation is intentional. Bazel resolves build-time dependencies and
runfiles; the runtime bundle supplies a fixed deployment layout; Python uses
normal Python package installation. Keeping these boundaries prevents a
deployment from accidentally depending on a developer checkout or stale Bazel
outputs.

## Native runtime bundle

Build the local Debian artifact:

```bash
cd /path/to/core
bazel build //:wheelos_core
sudo apt install ./bazel-bin/wheelos_core_1.0.0_all.deb
```

For a released build, install the matching Debian artifact from the release
instead. The installed layout is stable:

```text
/opt/wheelos_core/
  bin/        mainboard, cyber_launch, cyber_monitor, cyber_recorder
  lib/        Cyber runtime libraries
  resources/  Cyber configuration resources
  setup.bash  interactive-shell environment
```

Initialize each interactive shell before using an installed tool:

```bash
source /opt/wheelos_core/setup.bash
cyber_launch --help
cyber_recorder --help
cyber_monitor --help
```

`setup.bash` sets `CYBER_PATH` to the installed resource root, prepends only
the bundle's `bin/` and `lib/` directories, and preserves explicitly supplied
runtime settings such as `CYBER_DOMAIN_ID`, `CYBER_IP`, and `GLOG_*`. By
default, logs are written below
`${XDG_STATE_HOME:-$HOME/.local/state}/wheelos_core/log`, rather than the
read-only installation directory.

For a service manager, set these environment variables explicitly in the
service definition and execute the absolute path
`/opt/wheelos_core/bin/mainboard`; service managers do not source interactive
shell scripts. An explicit launch-file path does not require a source checkout
or checkout-relative `CYBER_PATH`.

The native package is a runtime bundle, not an installed C++ development SDK:
its public headers expose third-party development dependencies that are not
bundled. Build C++ applications through the Bzlmod source SDK below.

## Bazel source SDK

The public Bzlmod identity is `wheelos_core`; reusable BUILD files must use
`@wheelos_core`, not a caller-local alias such as `@core`.

Until the public registry release is published, downstream development can
consume a local checkout explicitly:

```starlark
# MODULE.bazel
module(name = "my_application", version = "0.1.0")

bazel_dep(name = "wheelos_core", version = "1.0.0")
```

```text
# .bazelrc
build --cxxopt=-std=c++17
build --host_cxxopt=-std=c++17
```

```python
# BUILD.bazel
load("@rules_cc//cc:defs.bzl", "cc_binary")

cc_binary(
    name = "my_node",
    srcs = ["my_node.cc"],
    deps = ["@wheelos_core//cyber:cyber"],
)
```

```cpp
#include "cyber/cyber.h"

int main(int argc, char** argv) {
  apollo::cyber::Init(argv[0]);
  auto node = apollo::cyber::CreateNode("my_node");
  apollo::cyber::WaitForShutdown();
  return 0;
}
```

Build with the local module override only during local development:

```bash
bazel build --override_module=wheelos_core=/path/to/core //:my_node
```

After a registry release exists, remove `--override_module`; Bzlmod will
resolve the immutable module version declared in `MODULE.bazel`. Do not add
the core checkout, its `bazel-bin` directory, or runfiles directories to
`PATH`, `LD_LIBRARY_PATH`, or `PYTHONPATH`.

Use the public tool targets during Bazel development:

```bash
bazel run --override_module=wheelos_core=/path/to/core \
  @wheelos_core//cyber/tools/cyber_launch:cyber_launch -- --help
```

`@wheelos_core//cyber:runtime_tools` is the aggregate target for supported
Bazel development tools. `mainboard` is a process launcher, not a library;
applications should link the `//cyber:cyber` facade instead.

## Python

Build all release artifacts together so the runtime and Python ABI come from
the same source revision:

```bash
bash scripts/release/build_release_artifacts.sh
python3 -m pip install artifacts/release/pycyber/pycyber-*.whl
```

Install a released, version-matched wheel in the same way. The wheel is the
supported Python installation API. For a Bazel Python application, depend on
`@wheelos_core//cyber/python/cyber_py3:cyber` instead of installing a wheel
inside the Bazel workspace.

Release tags are named `wheelos_core-v<module-version>`, for example
`wheelos_core-v1.0.0`. Untagged builds produce a deterministic
`1.0.0.dev...` Python prerelease and must not be presented as the final
`1.0.0` release.

## Verifying a checkout or release candidate

Run the repository-owned checks before publishing:

```bash
bash scripts/release/check_bzlmod_lockfile.sh --check
bash scripts/release/validate_downstream_bazel_sdk.sh
bash scripts/release/validate_runtime_bundle.sh
bash scripts/release/build_release_artifacts.sh --skip-baseline
```

The downstream check creates an independent Bzlmod workspace. The runtime
check installs the Debian artifact into a temporary root and confirms that
`setup.bash` exposes the packaged commands, rather than finding a source-tree
binary.
