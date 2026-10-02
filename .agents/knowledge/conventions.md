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
- Keep reusable utility dependencies at their narrow Bazel target boundaries:
  `//cyber/common:file`, `//cyber/common:log`, `//cyber/time:duration`,
  `//cyber/time:time`, and individual `//cyber/base:*` targets such as
  `atomic_rw_lock`, `bounded_queue`, `object_pool`, and `thread_pool`.
- The aggregate targets `//cyber/common`, `//cyber/base`, and `//cyber:cyber`
  are convenience/compatibility entry points, not lightweight dependencies.
  `//cyber:cyber` resolves to `//cyber:cyber_core`; avoid it for utility-only
  consumers and tests.
- `//cyber/time:clock` and `//cyber/timer` are runtime facilities: the clock
  uses Cyber configuration/global data, and the timer wheel uses the Cyber
  scheduler/task system. Keep them separate from the standalone time types and
  base synchronization/container targets.
- Preserve the existing C++ namespaces and header paths when refining BUILD
  dependencies; Bazel target boundaries can be improved without API renames.

## Sources

- `cyber/component/`
- `cyber/common/`
- `cyber/base/BUILD`
- `cyber/common/BUILD`
- `cyber/logger/BUILD`
- `cyber/time/BUILD`
- `cyber/timer/BUILD`
- `cyber/BUILD`
- `cyber/mainboard/`
- `cyber/proto/component_conf.proto`
- `cyber/proto/dag_conf.proto`
- `examples/common_component_example/`
- `examples/timer_component_example/`
