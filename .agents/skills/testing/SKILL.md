---
name: testing
description: Choose and run repository Bazel tests for runtime, transport, message, example, and integration changes. Use when changing code or tests and validation is needed.
---

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

## GPU zero-copy

- CPU-safe ownership/session checks:
  `//cyber/transport/nvsci:gpu_channel_manager_test`,
  `//cyber/transport/nvsci:gpu_channel_session_test`, and
  `//tests/integration_test:gpu_channel_ipc_session_test`.
- On a CUDA host, use `--config=cuda` for
  `//cyber/transport/nvsci:gpu_writer_reader_test`,
  `//tests/integration_test:gpu_writer_reader_ipc_test`,
  `//examples:gpu_classifier_kernel_test`, and
  `//tests/perf_test:gpu_zero_copy_perf_test`. Set
  `--test_env=CYBER_GPU_IPC_E2E=1` to enable the cross-process RTPS/CUDA IPC
  case; without it that test skips. These CUDA targets are
  `requires_cuda()`-gated so default CPU builds do not require a CUDA toolkit.
- GPU writer tests need a writable, user-private `XDG_RUNTIME_DIR` (or
  `HOME` fallback). For Bazel tests, create a private subdirectory under
  `TEST_TMPDIR`; the manager test does this itself. A cross-container writer
  reservation works only when both processes share the same lock directory.
- Treat the zero-copy performance target as a short regression/stress check,
  not as the two-hour stability or comparative SOTA acceptance gate.

## Sources

- `scripts/build.sh`
- `.github/copilot-instructions.md`
- `tests/integration_test/`
- `cyber/transport/nvsci/BUILD`
- `cyber/transport/nvsci/gpu_channel_manager_test.cc`
- `cyber/transport/nvsci/gpu_writer_reader_test.cc`
- The nearest `BUILD` file for each target
