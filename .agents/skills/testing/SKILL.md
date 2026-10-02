---
name: testing
description: Choose and run repository Bazel unit, runtime, transport, message, example, and integration tests, including the canonical CI baseline. Use when changing code or tests and validation is needed.
---

# Testing

## When

Read this when changing runtime, transport, message, example, or test code.

## Rules / Facts

- Prefer Bazel targets instead of invoking compilers directly.
- The canonical Ubuntu 22.04 CI build and test entrypoint is
  `bash scripts/release/ubuntu2204_baseline.sh`. It checks the release version
  and Bzlmod lockfile, builds `//cyber` and `//:wheelos_core`, and runs the
  curated unit and integration test matrix, including
  `//tests/integration_test:core_tool_matrix_tests`.
- To match GitHub Actions when running Python tests locally, use Python 3.10
  and an isolated `PYTHONUSERBASE` with `protobuf==5.29.5`; pass that same
  `PYTHONUSERBASE` to Bazel via `--test_env`. The CI setup is defined in
  `.github/workflows/core-ci.yml`.
- `bazel test //...` runs every Bazel test target and is broader than the
  curated CI baseline; use it when a full repository-wide test run is needed.
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
- Iceoryx tests use shared `/tmp` and `/dev/shm` resource names. If local tests
  fail because another user owns stale IPC resources, run them in an
  IPC-isolated container or namespace; do not remove shared host IPC files.

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
