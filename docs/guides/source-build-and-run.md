# Source build and run

Use this path when you are developing from a checkout, testing local changes, or running examples directly from the repository tree.

## Before you begin

This guide is for running from a repository checkout. If you are integrating `wheelos_core` into another Bazel project, see [Secondary development and integration](secondary-development.md) instead.

The repository uses Bzlmod and external registry resolution. Ensure network access to the configured Bazel registries (`https://bcr.wheelos.cn/` and the default Bazel BCR) before running the build commands below. If the environment is offline or air-gapped, use the vendor/offline build flow described in the repository README instead of a standard local build.

Install the Bazel build toolchain and repository dependencies first:

```bash
sudo bash scripts/deploy/build.sh
```

## 1. Build the project

```bash
bash scripts/build.sh
```

For narrow validation during development:

```bash
bazel build //cyber
bazel test //cyber/message/...
```

## 2. Use the source-tree runtime

```bash
source scripts/env/runtime.bash
```

This prepares `PATH`, `CYBER_PATH`, and repository-local runtime settings so the built tools and examples can run directly from the checkout.

## 3. Run the example

Build the example:

```bash
bazel build //examples/common_component_example/...
```

Start the component:

```bash
cyber_launch start examples/common_component_example/common.launch
```

Or launch directly with `mainboard`:

```bash
mainboard -d examples/common_component_example/common.dag
```

Then run the writers in separate terminals:

```bash
bazel run //examples/common_component_example:channel_test_writer
bazel run //examples/common_component_example:channel_prediction_writer
```

## Verify

The component receives messages and logs output in the terminal running `cyber_launch` or `mainboard`.

## Next

- [Package installation and run](package-installation.md)
- [Installation and setup](../getting-started/installation.md)
- [Quick start](../getting-started/quickstart.md)
