# Testing

## When

Read this when changing runtime, transport, message, example, or test code.

## Rules / Facts

- Prefer Bazel targets instead of invoking compilers directly.
- For small changes, run targets from the nearest `BUILD` file; message changes
  can use `bazel test //cyber/message/...`.
- After Fast DDS, RTPS, or examples changes, run the durable regression targets:
  `//tests/integration_test:examples_regression_tests`,
  `//cyber/transport/integration_test:rtps_transceiver_test`, and
  `//cyber/transport/rtps:rtps_test`.
- The complete native/Python runtime, transport, record/play, mainboard, and
  command-line tool acceptance matrix is
  `//tests/integration_test:core_tool_matrix_tests`.
- Run `source scripts/env/runtime.bash` before using tools from `bazel-bin`.
- If an integration test has a timing-sensitive failure, rerun only the failed
  target once and distinguish an intermittent failure from a regression.

## Sources

- `scripts/build.sh`
- `.github/copilot-instructions.md`
- `tests/integration_test/`
- The nearest `BUILD` file for each target
