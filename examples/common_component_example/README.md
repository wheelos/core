# Common Component Example

This example loads a component from a shared library and feeds it messages
from two writer processes.

## Build

```bash
bazel build //examples/common_component_example/...
```

## Run

```bash
export GLOG_alsologtostderr=1
```

Start the component in one terminal:

```bash
cyber_launch start examples/common_component_example/common.launch
```

Alternatively, start it directly from the DAG file:

```bash
mainboard -d examples/common_component_example/common.dag
```

Start the two writer nodes in separate terminals:

```bash
bazel run //examples/common_component_example:channel_test_writer
```

```bash
bazel run //examples/common_component_example:channel_prediction_writer
```

The component output appears in the terminal running `mainboard` or
`cyber_launch`.
