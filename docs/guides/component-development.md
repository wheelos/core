# Component development

## Before you begin

The repository's component model is based on `Component` and `TimerComponent` from the `cyber` runtime.

## Pattern

A component typically:

- declares readers or a timer interval
- implements `Init()`
- implements `Proc(...)` or `Proc()`
- registers itself via `CYBER_REGISTER_COMPONENT`

The canonical example is `examples/common_component_example`.

## Steps

### 1. Review the example files

- `examples/common_component_example/common_component_example.cc`
- `examples/common_component_example/common_component_example.h`
- `examples/common_component_example/common.dag`
- `examples/common_component_example/common.launch`

### 2. Build the example

```bash
bazel build //examples/common_component_example/...
```

### 3. Run the component

```bash
cyber_launch start examples/common_component_example/common.launch
```

Or:

```bash
mainboard -d examples/common_component_example/common.dag
```

## DAG shape

```protobuf
module_config {
  module_library: "../bazel-bin/examples/common_component_example/libcommon_component_example.so"
  components {
    class_name: "CommonComponentSample"
    config {
      name: "common"
      readers { channel: "/apollo/prediction" }
      readers { channel: "/apollo/test" }
    }
  }
}
```

The reader count must match the component arity.

## Timer variant

The timer example is in `examples/timer_component_example`:

```bash
bazel build //examples/timer_component_example/...
cyber_launch start examples/timer_component_example/timer.launch
```

This variant uses `timer_components` with an `interval` instead of input readers.

## Verify

The component loads from the shared library and processes input as configured in the DAG.

## Next

- [Build and run](build-and-run.md)
- [Quick start](../getting-started/quickstart.md)
- [Common component example](../../examples/common_component_example/README.md)
- [Timer component example](../../examples/timer_component_example/README.md)
