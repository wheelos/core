# Project Name

wheelos_core

# Overview

`wheelos_core` is the WheelOS runtime foundation for message passing and middleware coordination. The repository implements the Cyber RT stack used for publish/subscribe channels, service/client RPC, component lifecycle management, coroutine-style scheduling, topology discovery, shared-memory and RTPS transport, record/playback, native tools, and Python bindings.

The code is organized under `cyber/`, with the public C++ entry point exposed through `cyber/cyber.h`. The project is built with Bazel and ships native runtime tools such as `cyber_launch`, `cyber_monitor`, and `cyber_recorder`.

# Role in WheelOS

`wheelos_core` sits in the WheelOS Runtime layer and provides the message-oriented middleware runtime used by higher-level components and applications.

```text
WheelOS
 |
 +--- Runtime
      |
      +--- wheelos_core
           |
           +--- cyber runtime
           |    +--- Node / Channel / Service APIs
           |    +--- Scheduler and task execution
           |    +--- Transport: INTRA / SHM / RTPS
           |    +--- Topology and service discovery
           |    +--- Record, tools, and Python integration
           |
           +--- examples
           +--- docs and release tooling
```

# Architecture

```text
Application / Component
          |
          v
+-----------------------------------------------+
| Node API (CreateNode / Channel / Service)     |
+-----------------------------------------------+
          |
          +----------------------+------------------+
          |                      |
          v                      v
+-------------------+   +----------------------------+
| Scheduler / Task  |   | Component loader / lifecycle |
+-------------------+   +----------------------------+
          |
          v
+-----------------------------------------------+
| Topology manager / service discovery          |
+-----------------------------------------------+
          |
          v
+-----------------------------------------------+
| Transport layer                                |
| INTRA / SHM / RTPS / QoS / message routing    |
+-----------------------------------------------+
          |
          v
Subscriber / Receiver / Proc() execution
```

The actual build graph includes targets such as `//cyber:cyber`, `//cyber:cyber_core`, `//cyber/service_discovery`, `//cyber/transport`, `//cyber/record`, and the tool targets under `//cyber/tools/...`.

# Installation

## Build the toolchain and dependencies

```bash
sudo bash scripts/deploy/build.sh
```

This installs the Bazel build dependencies used by the repository.

## Build the project

```bash
bash scripts/build.sh
```

The repository default build entry point is `scripts/build.sh`, which runs Bazel with the CI configuration and `//cyber/...` by default. For narrower checks, the repository also documents:

```bash
bazel build //cyber
bazel test //cyber/message/...
```

## Install the runtime bundle

```bash
bazel build //:wheelos_core
sudo apt install ./bazel-bin/wheelos_core_1.0.0_all.deb
source /opt/wheelos_core/setup.bash
cyber_launch --help
```

The Debian package is the supported native runtime bundle. It installs commands such as `mainboard`, `cyber_launch`, `cyber_monitor`, and `cyber_recorder` under `/opt/wheelos_core`.

# Examples

The repository contains minimal example components under `examples/`. The clearest end-to-end example is `examples/common_component_example`.

```bash
cd /path/to/core
bazel build //examples/common_component_example/...
```

Start the component in one terminal:

```bash
cyber_launch start examples/common_component_example/common.launch
```

Or start it directly from the DAG file:

```bash
mainboard -d examples/common_component_example/common.dag
```

Then start the two writer nodes in separate terminals:

```bash
bazel run //examples/common_component_example:channel_test_writer
bazel run //examples/common_component_example:channel_prediction_writer
```

This example demonstrates loading a component from a shared library and feeding it messages from multiple writer processes.

There are additional related examples under `examples/timer_component_example`, but the common component example is the smallest repository-provided example with explicit build and run commands.

# Documentation

The repository separates operational documentation from generated API reference docs.

- [Documentation index](docs/README.md)
- [Installation and setup](docs/getting-started/installation.md)
- [Quick start](docs/getting-started/quickstart.md)
- [Build and run](docs/guides/build-and-run.md)
- [Component development](docs/guides/component-development.md)
- [Tools and monitoring](docs/guides/tools-and-monitoring.md)
- [Topology and transport](docs/guides/topology-and-transport.md)
- [Common issues](docs/troubleshooting/common-issues.md)
- [Generated API reference](docs/reference/api.md)
- [Doxygen/Sphinx documentation index](docs/doxy-docs/source/index.md)
- [C++ API](docs/doxy-docs/source/cpp-api.md)
- [Python API](docs/doxy-docs/source/python-api.md)
- [Terms](docs/doxy-docs/source/terms.md)
- [Common component example](examples/common_component_example/README.md)
- [Timer component example](examples/timer_component_example/README.md)
- [Project repository](https://github.com/wheelos/core)
- [Repository context index](.github/context/index.md)

The operational documentation is the recommended entry point for installation and runtime usage; the generated API docs remain the code-level reference.
