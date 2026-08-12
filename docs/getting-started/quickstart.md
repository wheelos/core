# Quick start

## Before you begin

Use the repository's `common_component_example` as the minimum runnable example.

## Steps

### 1. Build the example

```bash
bazel build //examples/common_component_example/...
```

### 2. Start the component

```bash
export GLOG_alsologtostderr=1
cyber_launch start examples/common_component_example/common.launch
```

Equivalent direct launch:

```bash
mainboard -d examples/common_component_example/common.dag
```

### 3. Run the writers

Open two separate terminals and run:

```bash
bazel run //examples/common_component_example:channel_test_writer
bazel run //examples/common_component_example:channel_prediction_writer
```

## Verify

The component receives messages and prints output in the terminal running `cyber_launch` or `mainboard`.

## Next

- [Common component example](../../examples/common_component_example/README.md)
- [Timer component example](../../examples/timer_component_example/README.md)
- [Build and run](../guides/build-and-run.md)
