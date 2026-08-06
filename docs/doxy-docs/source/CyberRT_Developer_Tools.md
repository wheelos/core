# Developer tools

The runtime bundle installs the native tools below in
`/opt/wheelos_core/bin`. In a source checkout, build the tools and source
`scripts/env/runtime.bash` instead.

```bash
source /opt/wheelos_core/setup.bash
# or, from a checkout:
source scripts/env/runtime.bash
```

## `cyber_monitor`

Inspect discovered nodes, channels, message types, and rates:

```bash
cyber_monitor
```

Press `f` to show channel frame rates. Use the monitor to confirm that the
expected readers and writers are discovered before debugging an application.

## `cyber_recorder`

Record active channels:

```bash
cyber_recorder record -o demo.record -c /wheelos/examples/chatter
```

Play a record file:

```bash
cyber_recorder play -f demo.record
```

The exact options depend on the installed version; use
`cyber_recorder --help` for the complete command set.

## `cyber_launch` and `mainboard`

Launch a component DAG:

```bash
cyber_launch start examples/common_component_example/common.launch
```

Launch a DAG directly:

```bash
mainboard -d examples/common_component_example/common.dag
```

`mainboard` loads registered shared-library components. It is a process
launcher, not a library dependency for application binaries.

## Other topology tools

The runtime bundle also provides `cyber_channel`, `cyber_node`, and
`cyber_service` through the Bazel target `//cyber:runtime_tools`. These tools
are useful for querying channels, nodes, and services in a running domain.

## Troubleshooting checklist

1. Source the environment in every terminal.
2. Confirm `CYBER_DOMAIN_ID` and `CYBER_IP` match across processes.
3. Run `cyber_monitor` and check that topology discovery is active.
4. Check `GLOG_alsologtostderr=1` for short diagnostic sessions.
5. Use an absolute record path when a service manager or another working
   directory starts the process.

Legacy visualization and ROS bag conversion workflows are not part of the
supported wheelos_core documentation.
