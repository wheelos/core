# Timer component example

This example demonstrates a `TimerComponent` that writes a `Driver` message to `/carstatus/channel` on a fixed interval.

## Build

```bash
bazel build //examples/timer_component_example/...
```

## Run

Start the component with the launch file:

```bash
cyber_launch start examples/timer_component_example/timer.launch
```

Alternatively, start it directly from the DAG file:

```bash
mainboard -d examples/timer_component_example/timer.dag
```

The component writes a message every 10 ms according to the `interval : 10` setting in `timer.dag`.

## Related files

- [timer.dag](timer.dag)
- [timer.launch](timer.launch)
- [timer_component_example.cc](timer_component_example.cc)
- [timer_component_example.h](timer_component_example.h)
