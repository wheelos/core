# Documentation

This repository has two documentation layers:

- Operational docs: install, run, and debug the runtime.
- Reference docs: generated API and framework detail.

## Start here

1. [Installation and setup](getting-started/installation.md)
2. [Quick start](getting-started/quickstart.md)
3. [Deployment options](guides/build-and-run.md)

## Guides

### Application workflows

- [Publish/Subscribe Basics](guides/pubsub-basics.md)
- [Service and Client](guides/service-client.md)
- [Parameter Service](guides/parameter-service.md)
- [Record and Replay](guides/record-and-replay.md)
- [Python Quick Start](guides/python-quickstart.md)
- [GPU zero-copy usage guide](guides/gpu-zero-copy.md)
- [Zero-Copy and Sensor Data](guides/zero-copy-and-sensor-data.md)
- [Component development](guides/component-development.md)

### Build, deployment, and integration

- [Source build and run](guides/source-build-and-run.md)
- [Package installation and run](guides/package-installation.md)
- [Secondary development and integration](guides/secondary-development.md)
- [Deployment options](guides/build-and-run.md)
- [Release and verification](guides/release-and-verification.md)
- [Release validation in a clean x86 container](guides/release-validation.md)

### Runtime operations and performance

- [Topology and transport](guides/topology-and-transport.md)
- [Tools and monitoring](guides/tools-and-monitoring.md)
- [Scheduler strategy discovery and optimization](guides/scheduler-optimization.md)
- [Performance testing](guides/performance-testing.md)
- [Common issues](troubleshooting/common-issues.md)

## Examples

- [Common component example](../examples/common_component_example/README.md)
- [Timer component example](../examples/timer_component_example/README.md)

## Reference

- [Generated API index](doxy-docs/source/index.md)
- [C++ API](doxy-docs/source/api/cppapi_index.rst)
- [Python API](doxy-docs/source/api/pythonapi_index.rst)
- [C++ API guide](doxy-docs/source/cpp-api.md)
- [Python API guide](doxy-docs/source/python-api.md)
- [Terms](doxy-docs/source/terms.md)

## Structure

Use operational docs for the primary workflow. Use generated API docs for implementation details, signatures, and deeper runtime concepts.
