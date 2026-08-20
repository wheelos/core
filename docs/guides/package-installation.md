# Package installation and run

Use this path when you want to run the runtime from a published native bundle rather than from the repository checkout.

## Before you begin

This guide is for running the installed runtime bundle. If you are integrating `wheelos_core` into another Bazel project, see [Secondary development and integration](secondary-development.md) instead.

Build the native bundle:

```bash
bazel build //:wheelos_core
DEB="$(find bazel-bin -maxdepth 1 -name 'wheelos_core_*.deb' -print -quit)"
sudo apt install "$DEB"
```

Then load the installed runtime environment:

```bash
source /opt/wheelos_core/setup.bash
```

## Verify the installation

```bash
cyber_launch --help
mainboard --help
cyber_monitor --help
cyber_recorder play --help
```

If these commands succeed, the installed runtime is available in the current shell.

## Run the packaged runtime

Launch a DAG through the installed tools:

```bash
cyber_launch start examples/common_component_example/common.launch
```

Or run the direct component entry point:

```bash
mainboard -d examples/common_component_example/common.dag
```

For a quick smoke test, use the repository example as described in the quick-start guide.

## Next

- [Source build and run](source-build-and-run.md)
- [Installation and setup](../getting-started/installation.md)
- [Quick start](../getting-started/quickstart.md)
