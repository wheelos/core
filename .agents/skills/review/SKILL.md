# Review

## When

Read this when reviewing code, configuration, or BUILD changes, or preparing a
change for submission.

## Rules / Facts

- Inspect the diff first, then verify behavior through callers, configuration,
  and adjacent tests.
- Check that Bazel targets, dependencies, test coverage, and runtime
  configuration remain synchronized.
- Check constraints such as component arity versus DAG readers, path
  resolution, and topology-name uniqueness.
- Do not treat unverified architecture assumptions as facts; use source,
  configuration, or tests as evidence.
- Report only reproducible or clearly evidenced issues, with location and
  impact.

## Sources

- `.github/copilot-instructions.md`
- `cyber/component/`
- `cyber/mainboard/`
- `cyber/proto/`
- `examples/common_component_example/`
- `examples/timer_component_example/`
