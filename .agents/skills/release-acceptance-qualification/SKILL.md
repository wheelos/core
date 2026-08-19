# Release Acceptance and Qualification

## When

Use this skill when qualifying a candidate release, validating packaging and
installation integrity, running runtime/examples/tools verification, or
conducting stress and performance acceptance gates for `wheelos/core`.

## Three-Part Release Acceptance Framework

Release qualification has three mandatory, independent gates. A release is
approved only when all three parts pass completely:

1. **Part 1: Packaging & Installation Verification (打包发布与安装验证)**
2. **Part 2: Runtime, Examples & Tools Verification (运行验证：示例与工具链)**
3. **Part 3: Stress, Performance & Leak Gate (压力、性能与内存泄漏测试)**

---

## Part 1: 打包发布与安装验证 (Packaging & Installation)

### Objectives
- Ensure the Bzlmod module lockfile is synchronized and reproducible.
- Build native runtime packages (`.deb` bundle, SDK headers, core libraries) and Python binary (`.whl`) and source (`.tar.gz`) distributions.
- Validate customer installation into clean, isolated environments without dependency on the build workspace.
- Confirm downstream Bzlmod SDK consumer projects can import and build against `@wheelos_core`.

### Verification Steps & Commands

1. **Lockfile Integrity:**
   ```bash
   bash scripts/release/check_bzlmod_lockfile.sh --check
   ```
2. **Artifact Packaging:**
   ```bash
   bash scripts/release/build_release_artifacts.sh \
     --outdir artifacts/release \
     --distdir /tmp/cache/ \
     --skip-baseline
   ```
   Artifacts generated:
   - `artifacts/release/core/wheelos_core_*.deb`: Native runtime bundle.
   - `artifacts/release/pycyber/*.whl` and `*.tar.gz`: Python package distributions.
   - `artifacts/release/pycyber/SHA256SUMS` and `artifacts/release/manifest.txt`.

3. **Customer Native Installation Check:**
   - Unpack/install `.deb` package into an isolated root directory.
   - Source `/opt/wheelos_core/setup.bash`.
   - Verify tool binaries execute:
     ```bash
     mainboard --help
     cyber_recorder --help
     cyber_monitor -h
     cyber_launch --help
     ```
   - Verify shared libraries exist under `/opt/wheelos_core/lib/libcyber_core.so`.

4. **Python Installation & Import Check:**
   - In a fresh, isolated Python 3.10 virtualenv:
     ```bash
     pip install artifacts/release/pycyber/*.whl
     python3 -c "
     import pycyber
     from pycyber import cyber, record
     from pycyber.proto import record_pb2
     assert cyber is not None and record is not None
     "
     ```

5. **Downstream Bazel SDK Consumer Check:**
   ```bash
   bash scripts/release/validate_downstream_bazel_sdk.sh
   ```

---

## Part 2: 运行验证：示例与工具链 (Runtime, Examples & Tools)

### Objectives
- Verify that runtime initialization, pub/sub, parameter services, and coroutine scheduling work across all transport modes (INTRA, SHM, RTPS).
- Validate all standalone C++ examples (`talker`, `listener`, `record`, `service`, `paramserver`).
- Validate all Python examples (`examples_smoke_test`).
- Validate Component DAG loading and timer components via `mainboard`.
- Verify the Cyber RT CLI tools (`cyber_channel`, `cyber_node`, `cyber_service`, `cyber_recorder`, `cyber_monitor`, `cyber_launch`).

### Verification Steps & Commands

1. **Python Examples Smoke Test:**
   ```bash
   bazel test --config=ci //cyber/python/cyber_py3/examples:examples_smoke_test --test_output=errors
   ```

2. **C++ Examples & Regression Suite:**
   ```bash
   bazel test --config=ci \
     //cyber/message/... \
     //cyber/transport/integration_test:rtps_transceiver_test \
     //cyber/transport/rtps:rtps_test \
     //tests/integration_test:examples_regression_tests \
     --test_output=errors
   ```
   This regression test suite validates:
   - `cyber_lifecycle_test`: Framework init/shutdown lifecycle.
   - `cyber_tools_discovery_test`: Cross-process discovery, channel echo/hz/bw, service calls, recorder record/split/recover/play, monitor, launch.
   - `examples_integration_test`: Binary payload integrity with embedded `\0` bytes across transports.
   - `mainboard_integration_test`: Dynamic component loading via `.dag` descriptors.
   - `record_play_test`: Record file writing, indexing, chunking, and playback.

3. **Standalone Binary Sanity:**
   ```bash
   bazel build --config=ci //examples:talker //examples:listener //examples:record
   ```

---

## Part 3: 压力、性能与内存泄漏测试 (Stress, Performance & Leaks)

### Objectives
- Measure throughput, latency (mean, p50, p95, p99, max), and jitter across Intra-Process, Inter-Process SHM, and RTPS transports.
- Ensure 0 packet loss (`total_loss == 0`) and 100% case success rate across payload matrices (64B to 10MB).
- Validate zero-copy large payload performance (SHM / Iceoryx zero-copy copy count == 0).
- Run high-concurrency stress testing (1:N fanout, N:1 fanin, burst matrices, multi-client RPC).
- Check memory leaks under Valgrind (0 definitely lost bytes, 0 indirectly lost bytes).

### Verification Steps & Commands

1. **Performance Baseline Benchmark Matrix:**
   ```bash
   bash scripts/release/run_performance_baseline.sh \
     --outdir artifacts/release-acceptance/performance \
     --quick
   ```
   - Inspect `artifacts/release-acceptance/performance/baseline.json`.
   - Gate criteria:
     ```python
     import json
     data = json.load(open("artifacts/release-acceptance/performance/baseline.json"))
     results = data["results"]
     assert all(r["success"] for r in results), "Failed performance cases"
     assert all(r["reliability"]["total_loss"] == 0 for r in results), "Packet loss detected"
     ```

2. **Concurrency & Stress Matrix:**
   ```bash
   bazel test --config=ci //tests/integration_test:examples_stress_test --test_output=errors
   ```

3. **Valgrind Memory Leak Matrix:**
   ```bash
   bash scripts/release/run_cyber_memory_leak_matrix.sh \
     --outdir artifacts/release-acceptance/memory-leak
   ```
   ```bash
   bash scripts/release/run_memory_leak_check.sh \
     --outdir artifacts/release-acceptance/memory-leak-record
   ```
   - Gate criteria: `definitely lost: 0 bytes in 0 blocks`, `indirectly lost: 0 bytes in 0 blocks`.

4. **Long-Running Pub/Sub Stress (Release Validation):**
   ```bash
   bazel build --config=ci //tests/perf_test:benchmark_monitor
   bazel-bin/tests/perf_test/benchmark_monitor \
     --output_json=artifacts/release-acceptance/stability/long-run.json \
     --enable_long_run=true \
     --long_run_seconds=7200 \
     --long_run_frequency_hz=200 \
     --long_run_payload_bytes=1024
   ```

---

## Unified Qualification Runner

To execute the entire 3-part release qualification workflow and produce a comprehensive markdown & JSON acceptance report:

```bash
bash scripts/release/run_release_acceptance.sh \
  --outdir artifacts/release-acceptance \
  --quick
```

Options:
- `--stage [all|1|2|3|packaging|runtime|perf]`: Run specific acceptance stages.
- `--quick`: Run optimized benchmark bandwidth matrices for rapid smoke qualification.
- `--skip-auditwheel`: Skip manylinux wheel repair if environment lacks auditwheel/patchelf.

## Artifacts & Reporting Deliverables

The acceptance run produces the following qualification artifacts:
- `artifacts/release-acceptance/summary.md`: Top-level qualification report matrix.
- `artifacts/release/core/*.deb`: Shipped native Debian runtime package.
- `artifacts/release/pycyber/*.whl`: Shipped Python wheel distributions.
- `artifacts/release-acceptance/performance/baseline.json` & `summary.md`: Performance benchmark dataset.
- `artifacts/release-acceptance/memory-leak/valgrind-*.log`: Valgrind memory leak logs.

## Sources

- `scripts/release/run_release_acceptance.sh`
- `scripts/release/build_release_artifacts.sh`
- `scripts/release/validate_runtime_bundle.sh`
- `scripts/release/validate_downstream_bazel_sdk.sh`
- `scripts/release/run_performance_baseline.sh`
- `scripts/release/run_cyber_memory_leak_matrix.sh`
- `scripts/release/run_memory_leak_check.sh`
- `tests/integration_test/examples_stress_test.cc`
- `tests/integration_test/cyber_tools_discovery_test.py`
