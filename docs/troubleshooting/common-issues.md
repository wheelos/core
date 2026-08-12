# Common issues

## `cyber_launch` or `mainboard` is not found

Load the runtime environment before running the native tools:

```bash
source /opt/wheelos_core/setup.bash
```

If you are running from a source checkout instead of the installed bundle, use:

```bash
source scripts/env/runtime.bash
```

## Bazel build dependencies are missing

Install the repository build dependencies:

```bash
sudo bash scripts/deploy/build.sh
```

## Example build fails

Build the specific example first:

```bash
bazel build //examples/common_component_example/...
```

## The component does not start or cannot find the shared library

The example DAG references the built shared library under `bazel-bin`. Confirm that you have built the target and that the DAG still points to the expected output path.

For the repository example, the shared library path is declared in `examples/common_component_example/common.dag` and matches the Bazel output layout used by the example build.

## Logger output is missing

Set the log environment before starting the example:

```bash
export GLOG_alsologtostderr=1
```

## Related guides

- [Installation guide](../getting-started/installation.md)
- [Quick start](../getting-started/quickstart.md)
