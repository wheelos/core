# Context index

## Build and release

- `design/build-release-architecture.md` — Bzlmod-first build/release structure, lockfile policy, and artifact flow.
- `design/bzlmod-readiness-assessment.md` — current Bazel rule replacement status in `bazel/` and Bzlmod readiness gaps.
- `design/record-io-uring-migration.md` — Design proposal for migrating the record file subsystem to io_uring, with benchmark methodology and baseline performance comparisons.
- `design/pod-iceoryx-vs-protobuf-benchmark.md` — POD (Iceoryx) vs. Protobuf (shared memory) benchmark, including Flood, Payload Sweep, Fanout, and Tail Latency methodologies and results.
- `skills/bzlmod-build-release.md` — concrete commands for Ubuntu 22.04 baseline validation and release packaging.
- `skills/offline-vendor-validation.md` — lockfile, vendor registry, packaging, and network-isolated build acceptance procedure.

## Middleware roadmap

- `roadmaps/middleware-evolution.md` — Fast DDS 2.14.x stabilization plan, zero-copy evolution stages, and recommended next milestones.
- `roadmaps/runtime-contract-evolution.md` — Semantic, lifecycle, compatibility,
  recovery, low-overhead observability, determinism, and devtools roadmap.
- `design/runtime-contract-rollout-plan.md` — compatibility-first implementation
  work packages, invariant behavior, validation gates, and rollback boundaries.
- `../../docs/guides/runtime-contract-v1.md` — characterized Writer/Reader,
  queue, callback, and lifecycle behavior with test-backed unsupported boundaries.
- `design/runtime-metrics-plan.md` — proposed runtime metrics definitions, instrumentation points, staged delivery, and acceptance criteria.
- `design/runtime-metrics-usage.md` — current opt-in local runtime metrics configuration, output, and limitations.
- `design/runtime-metrics-performance.md` — hot-path and Node pub/sub comparisons, usage and limits of their conclusions.
