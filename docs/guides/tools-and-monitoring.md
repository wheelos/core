# Tools and monitoring

## Before you begin

The repository provides native tools for starting, monitoring, and recording Cyber runtime processes.

## Common commands

From the installed bundle or an active checkout runtime environment:

```bash
cyber_launch --help
mainboard --help
cyber_monitor --help
cyber_recorder play --help
```

## Steps

### 1. Load the runtime environment

Source-tree runtime:

```bash
source scripts/env/runtime.bash
```

Installed bundle:

```bash
source /opt/wheelos_core/setup.bash
```

### 2. Start an example

```bash
cyber_launch start examples/common_component_example/common.launch
```

### 3. Inspect runtime behavior

```bash
cyber_monitor --help
cyber_recorder play --help
```

## Verify

Use `cyber_monitor` and `cyber_recorder` to inspect runtime state and recorded data while debugging a running example.

## Next

- [Deployment options](build-and-run.md)
- [Installation guide](../getting-started/installation.md)
- [Quick start](../getting-started/quickstart.md)
