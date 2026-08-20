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
- [Source build and run](../guides/source-build-and-run.md)
- [Package installation and run](../guides/package-installation.md)
- [Secondary development and integration](../guides/secondary-development.md)
- [Release and verification](../guides/release-and-verification.md)
- [Performance testing](../guides/performance-testing.md)
- [Publish/Subscribe Basics](../guides/pubsub-basics.md)
- [Python Quick Start](../guides/python-quickstart.md)
