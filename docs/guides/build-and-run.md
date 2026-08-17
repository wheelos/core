# Build and run

## Before you begin

This is the standard repository build and runtime startup flow.

## Steps

### 1. Install build dependencies

```bash
sudo bash scripts/deploy/build.sh
```

### 2. Build the project

```bash
bash scripts/build.sh
```

Focused validation:

```bash
bazel build //cyber
bazel test //cyber/message/...
```

### 2a. Optional: offline vendor build mode

The default release flow is unchanged. `--config=vendor` is an opt-in mode for air-gapped or reproducible offline builds. It keeps the standard build path intact while creating a vendored Bazel dependency snapshot for use with the repository source tree.

```bash
bazel build --config=vendor --nobuild //:wheelos_core
bash scripts/release/build_vendor_bundle.sh --outdir artifacts/vendor
```

This bundle includes the repository snapshot plus the vendored external dependency cache under `vendor/bazel`; it is for offline rebuilds, not as a substitute for the normal `.deb` / wheel publishing flow.

### 3. Use the source-tree runtime

```bash
source scripts/env/runtime.bash
```

This prepares `PATH`, `CYBER_PATH`, and repo-local logging paths.

### 4. Install the native bundle

```bash
bazel build //:wheelos_core
sudo apt install ./bazel-bin/wheelos_core_1.0.0_all.deb
source /opt/wheelos_core/setup.bash
```

Verify:

```bash
cyber_launch --help
mainboard --help
cyber_monitor --help
cyber_recorder --help
```

### 5. Run the example

```bash
bazel build //examples/common_component_example/...
cyber_launch start examples/common_component_example/common.launch
```

Or:

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

- [Installation guide](../getting-started/installation.md)
- [Quick start](../getting-started/quickstart.md)
- [Component development](component-development.md)
