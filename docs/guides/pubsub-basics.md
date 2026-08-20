# Publish/Subscribe Basics

This guide introduces the core Cyber RT communication pattern: publishers write messages to a channel, and subscribers receive them.

## What you will build

- One component subscribes to a topic
- Two writer processes send messages to that same topic
- The component prints the received messages

This is the smallest runnable example in this repository:

- `examples/common_component_example`

## Prerequisites

Build the dependencies and load the runtime environment:

```bash
sudo bash scripts/deploy/build.sh
source scripts/env/runtime.bash
```

## Build the example

```bash
bazel build //examples/common_component_example/...
```

## Run the component

Start the component in one terminal:

```bash
cyber_launch start examples/common_component_example/common.launch
```

Or start it directly from the DAG file:

```bash
mainboard -d examples/common_component_example/common.dag
```

## Run writers

Open two additional terminals and run:

```bash
bazel run //examples/common_component_example:channel_test_writer
bazel run //examples/common_component_example:channel_prediction_writer
```

## Verify

The component should receive messages and print output in the terminal running `cyber_launch` or `mainboard`.

## Key concepts

- Channel: a named message stream
- Publisher: writes messages to a channel
- Subscriber: reads messages from a channel
- Node: runtime process that owns publishers and subscribers

## Next steps

- [Service and client](service-client.md)
- [Parameter service](parameter-service.md)
- [Record and replay](record-and-replay.md)
