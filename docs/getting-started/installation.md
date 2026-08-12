# Installation and setup

## Before you begin

Use either the repository source-build flow or the packaged runtime bundle.

## Steps

### 1. Install build dependencies

```bash
sudo bash scripts/deploy/build.sh
```

### 2. Build from source

```bash
bash scripts/build.sh
```

Useful narrow checks:

```bash
bazel build //cyber
bazel test //cyber/message/...
```

### 3. Install the runtime bundle

```bash
bazel build //:wheelos_core
sudo apt install ./bazel-bin/wheelos_core_1.0.0_all.deb
```

Load the installed runtime environment:

```bash
source /opt/wheelos_core/setup.bash
cyber_launch --help
mainboard --help
cyber_monitor --help
cyber_recorder --help
```

### 4. Run directly from a checkout

If you want to run binaries from the source tree instead of the installed bundle:

```bash
source scripts/env/runtime.bash
```

This sets `CYBER_PATH`, adds Bazel outputs to `PATH`, and configures the repo-local runtime environment.

## Verify

The minimal validation is:

```bash
cyber_launch --help
```

If it succeeds, the runtime is available in the current shell.

## Next

- [Quick start](quickstart.md)
- [Build and run](../guides/build-and-run.md)
- [Common component example](../../examples/common_component_example/README.md)
