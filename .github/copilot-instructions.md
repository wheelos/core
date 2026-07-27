# Copilot instructions for `wheelos/core`

## Build, test, and lint

- This repo is Bazel-first. Prefer Bazel commands over ad-hoc compiler invocations. `.bazelrc` enables Bzlmod, forces C++17, and points Bazel at custom registries (`https://bcr.wheelos.cn/` and `https://raw.githubusercontent.com/wheelos/bazel-central-registry/fastdds-2.14.x`), so first builds may need network access.
- Install the Bazel build environment with:
  ```bash
  sudo bash scripts/deploy/build.sh
  ```
- Full build:
  ```bash
  bash scripts/build.sh
  ```
  That script currently runs:
  ```bash
  bazel build --config=ci //cyber/... --distdir=/tmp/cache/
  ```
- The canonical Ubuntu 22.04 CI / release-validation baseline is:
  ```bash
  bash scripts/release/ubuntu2204_baseline.sh
  ```
- Keep `MODULE.bazel.lock` checked in and in sync. Use:
  ```bash
  bash scripts/release/check_bzlmod_lockfile.sh --check
  bash scripts/release/check_bzlmod_lockfile.sh --update
  ```
- The release artifact entrypoint is:
  ```bash
  bash scripts/release/build_release_artifacts.sh
  ```
  It runs the Ubuntu 22.04 baseline by default, then collects `//:wheelos_core` outputs and the `pycyber` wheelhouse into a single artifact directory.
- CI also uses a narrower top-level build:
  ```bash
  bazel build //cyber
  ```
- Run the test suite used in CI:
  ```bash
  bazel test //cyber/message/...
  ```
- Run a single test target with Bazel target syntax:
  ```bash
  bazel test //cyber/message:message_header_test --test_output=errors
  ```
  In general, tests are defined as `cc_test` targets in the nearest `BUILD` file and should be run as `bazel test //path/to/package:target_name`.
- After Fast DDS / RTPS / `cyber/examples` changes, run the durable regression suite:
  ```bash
  bazel test \
    //cyber/examples/integration_test:examples_regression_tests \
    //cyber/transport/integration_test:rtps_transceiver_test \
    //cyber/transport/rtps:rtps_test \
    --test_output=errors
  ```
- `//cyber/examples/integration_test:examples_regression_tests` is the long-lived example coverage entrypoint. It covers binary payload integrity (including embedded `\0` bytes for the Fast CDR string patch path), 1 writer / N readers fanout, N writers / 1 reader fanin, multi-client service round trips, payload-size pub/sub stress, and service burst matrices. Keep these tests discovery-gated and rely on bounded completion plus low-bar throughput assertions instead of brittle absolute latency checks.
- Before running built tools or examples from `bazel-bin`, source the runtime environment:
  ```bash
  source scripts/env/runtime.bash
  ```
  The README examples then run binaries like:
  ```bash
  ./bazel-bin/cyber/examples/listener
  ./bazel-bin/cyber/examples/talker
  ```
- Lint scripts exist under `scripts/lint/`:
  ```bash
  bash scripts/lint/lint.sh --cpp
  bash scripts/lint/lint.sh --py
  bash scripts/lint/lint.sh --sh
  bash scripts/lint/lint.sh -a
  ```
  These scripts are inherited from Apollo tooling and currently source `scripts/apollo.bashrc` and `scripts/apollo_base.sh`, which are not present in this fork. Treat them as intended entrypoints, but confirm/fix their environment before relying on them.
- Persistent repository context lives under `.github/context/`. Read `.github/context/index.md` before changing build/release flow or middleware roadmap assumptions.

## High-level architecture

- `cyber/` is the core runtime. `cyber/cyber.h` is the public convenience header; `cyber/init.cc` performs framework startup and shutdown. Initialization wires together logging, the scheduler, service discovery, transport, the timing wheel, task management, and mock clock subscription.
- `Node` is the main application-facing abstraction. It exposes pub/sub and RPC-style APIs:
  - channel-based pub/sub goes through `NodeChannelImpl`
  - service/client RPC goes through `NodeServiceImpl`
- `mainboard` is the process launcher for component-based systems. `cyber/mainboard/mainboard.cc` initializes Cyber RT, then `ModuleController` reads DAG config files, resolves module paths, loads shared libraries with `ClassLoaderManager`, instantiates registered `ComponentBase` implementations, and keeps them running until shutdown.
- Component wiring is configuration-driven:
  - `cyber/proto/dag_conf.proto` defines `module_config`, `module_library`, `components`, and `timer_components`
  - `cyber/proto/component_conf.proto` defines component names, reader channels, config files, flag files, and timer intervals
  - `cyber/examples/common_component_example/` and `cyber/examples/timer_component_example/` are the best end-to-end references for how a component class, its shared library, and its `.dag` file fit together
- Message transport is layered:
  - `cyber/transport/transport.h` can create INTRA, SHM, RTPS, or HYBRID transmitters/receivers
  - HYBRID is the default
  - default QoS is injected when a role does not provide one
- Topology/discovery is handled separately from transport:
  - `cyber/service_discovery/topology_manager.*` tracks nodes, channels, and services through `NodeManager`, `ChannelManager`, and `ServiceManager`
  - it uses an RTPS participant to broadcast topology joins/leaves and to notify listeners about graph changes
- Scheduling is coroutine-based. `cyber/scheduler/` owns task creation/dispatch, and component initialization typically turns readers into scheduled tasks rather than invoking user logic directly from transport callbacks.

## Key conventions

- New runtime components should usually inherit from `Component<M0, ...>` or `TimerComponent`, override `Init()` and `Proc(...)`/`Proc()`, and end with:
  ```cpp
  CYBER_REGISTER_COMPONENT(YourComponent)
  ```
  The framework owns `Process(...)`; component authors implement `Proc(...)`.
- Component arity and config must match. A `Component<M0, M1>` expects at least two `readers` entries in its `ComponentConfig`; the templated `Component` specializations enforce this during `Initialize`.
- `ModuleController` resolves DAG paths from the current directory, the repo work root, or the default `dag/` directory. `module_library` paths inside DAG files may be absolute or repo-relative. Do not assume the loader only handles one style.
- `config_file_path` and `flag_file_path` inside component configs are resolved relative to `common::WorkRoot()` when they are not absolute paths. Keep new config references compatible with that resolution behavior.
- Node/topology names are expected to be unique. `Node` warns and rejects duplicate readers on the same channel within a node, and the API comments explicitly call out duplicate topo names as invalid.
- Keep tests and libraries in the nearest Bazel package. The repository convention is to colocate `cc_library` / `cc_binary` / `cc_test` targets with the code in the local `BUILD` file rather than centralizing tests elsewhere.
- BUILD files should use the standard Bazel rule sets directly; there is no repo-local `cpplint()` macro anymore.
- The example DAGs still use Apollo-style absolute library paths under `/apollo/bazel-bin/...`. If you update or add DAG examples in this fork, verify the shared-library path matches the actual Bazel output layout for this repository instead of assuming those upstream paths are correct.

## Middleware roadmap

- The current stable transport baseline is Fast DDS 2.14.6 + Fast CDR 2.2.7 with the carried Fast CDR embedded-null string serialization patch. Treat that patch as intentional until protobuf-binary-string compatibility is no longer required.
- Prefer staged evolution over a direct Fast DDS 3.x jump:
  1. stabilize current RTPS + example regression coverage
  2. improve in-process and same-host zero-copy/data-sharing paths
  3. add heterogeneous buffer-handle negotiation only after the shared-memory contracts are explicit and testable
- For roadmap, build/release, and skill details, use:
  - `.github/context/roadmaps/middleware-evolution.md`
  - `.github/context/design/build-release-architecture.md`
  - `.github/context/skills/bzlmod-build-release.md`
