# Secondary Development and Integration

This guide is intentionally a decision guide, not a duplicate of the build/install workflow. For setup commands, see the deployment and installation documents. This page explains how downstream projects should consume `wheelos_core` in the right mode.

## 1. Choose the integration model

Use the following model depending on your goal:

- Local source checkout: active development, debugging, or validating unreleased changes
- Released Bazel dependency: standard downstream integration with a stable version
- Installed runtime bundle: system deployment or runtime-only consumers
- Python wheel: Python applications that do not build from the repository checkout

## 2. Local source checkout with Bazel

This is the right choice when you are actively developing `wheelos_core` or validating a local branch in another workspace.

The repository supports Bzlmod downstream consumption and includes a canonical validation pattern in `scripts/release/validate_downstream_bazel_sdk.sh`.

### Minimal `MODULE.bazel`

```python
module(name = "my_app", version = "0.0.0")

bazel_dep(name = "wheelos_core", version = "1.0.0")
```

### Minimal `BUILD.bazel`

```python
load("@rules_cc//cc:defs.bzl", "cc_binary")
load("@rules_python//python:defs.bzl", "py_binary")

cc_binary(
    name = "cpp_consumer",
    srcs = ["cpp_consumer.cc"],
    deps = ["@wheelos_core//cyber:cyber"],
)

py_binary(
    name = "python_consumer",
    srcs = ["python_consumer.py"],
    deps = ["@wheelos_core//cyber/python/cyber_py3:cyber"],
)
```

### Local override for development

When validating an untagged local change, override the module directly:

```bash
bazel build \
  --override_module=wheelos_core=/path/to/core \
  //:cpp_consumer \
  //:python_consumer
```

Use this pattern when:

- you are testing unreleased fixes
- you are integrating a local branch into another workspace
- you want to validate compatibility before a release tag is published

## 3. Released dependency via Bazel

Use a versioned `bazel_dep` when your project depends on a released `wheelos_core` package rather than the current checkout.

This is the recommended path for production downstream applications and libraries.

```python
module(name = "my_app", version = "0.0.0")

bazel_dep(name = "wheelos_core", version = "1.0.0")
```

The downstream project should then depend on the public targets, such as:

- `@wheelos_core//cyber:cyber`
- `@wheelos_core//cyber/python/cyber_py3:cyber`

## 4. Installed runtime bundle

Use the installed runtime bundle when the application is meant to run the packaged system binaries rather than build against the source tree.

This mode is appropriate when:

- the runtime is deployed as a native bundle
- operators or services run `cyber_launch`, `mainboard`, `cyber_monitor`, `cyber_recorder`
- you want a stable deployed runtime rather than a checkout-based environment

The runtime bundle exposes the toolchain through the environment script and installs runtime libraries under the standard bundle paths. Its installation and runtime setup are documented in the deployment documents.

## 5. Public API consumption

### C++ consumers

Use the generated public header and library path from the installed bundle or from the Bazel target dependency.

```cpp
#include "cyber/cyber.h"
```

The bundle installs the runtime libraries under `/opt/wheelos_core/lib` and the public headers under `/usr/local/include`.

### Python consumers

Use the public Python binding import path:

```python
from cyber.python.cyber_py3 import cyber
```

For packaged delivery, prefer the released `pycyber` wheel instead of relying on an ad hoc source-tree environment.

## 6. Decision guide

Choose the path as follows:

- If you are developing `wheelos_core` itself: use a local checkout and `--override_module`
- If you are consuming a released version: use `bazel_dep(name = "wheelos_core", version = "...")`
- If you are running the runtime as a deployed installation: use the installed bundle
- If you are writing a Python app: use the released `pycyber` wheel or the Bazel Python target

## 7. Common mistakes

- mixing a source-tree runtime with an installed bundle in the same shell
- using an old checkout without overriding the module when testing a local patch
- building Python code without the expected `cyber.python.cyber_py3` import path
- treating a local checkout as if it were a published release artifact

## Next steps

- [Deployment options](build-and-run.md)
- [Source build and run](source-build-and-run.md)
- [Package installation and run](package-installation.md)
- [Python quick start](python-quickstart.md)
