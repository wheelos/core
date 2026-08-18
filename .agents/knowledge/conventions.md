# Conventions

## When

Read this when adding components, message channels, DAGs, tests, or Bazel
targets.

## Rules / Facts

- New components usually inherit from `Component<M0, ...>` or
  `TimerComponent` and use `CYBER_REGISTER_COMPONENT(...)` for registration.
- A component template's message arity must match the configured number of
  `readers`.
- Keep libraries, binaries, and tests in the nearest relevant `BUILD` file.
- Non-absolute `config_file_path` and `flag_file_path` values are resolved
  relative to `common::WorkRoot()`.
- A DAG's `module_library` may be absolute or repository-relative; verify the
  actual Bazel output path when adding examples.
- Node and topology names should be unique; duplicate reader channels in one
  node are rejected.

## Sources

- `cyber/component/`
- `cyber/common/`
- `cyber/mainboard/`
- `cyber/proto/component_conf.proto`
- `cyber/proto/dag_conf.proto`
- `examples/common_component_example/`
- `examples/timer_component_example/`
