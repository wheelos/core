# Repository Knowledge

This is the durable knowledge entrypoint. Add new reusable facts here with source
evidence; keep task procedures in `../skills/` and temporary work in `../notes/`.

- [Architecture](architecture.md): runtime ownership and lifecycle boundaries.
- [Conventions](conventions.md): source and Bazel conventions.
- [Troubleshooting](troubleshooting.md): known failure localization.

## Designs and evidence

- [Build/release architecture](design/build-release-architecture.md)
- [Bzlmod readiness assessment](design/bzlmod-readiness-assessment.md)
- [Record io_uring migration](design/record-io-uring-migration.md)
- [POD versus protobuf benchmark](design/pod-iceoryx-vs-protobuf-benchmark.md)
- [Runtime contract rollout](design/runtime-contract-rollout-plan.md)
- [Runtime metrics plan](design/runtime-metrics-plan.md)
- [Runtime metrics usage](design/runtime-metrics-usage.md)
- [Runtime metrics performance](design/runtime-metrics-performance.md)
- [Offline vendor validation report](reports/offline-vendor-validation.md)

## Roadmaps and public contracts

- [Middleware evolution](roadmaps/middleware-evolution.md)
- [Runtime contract evolution](roadmaps/runtime-contract-evolution.md)
- [Public runtime contract](../../docs/guides/runtime-contract-v1.md)

Repository knowledge is maintained here. Public API documentation remains in
`docs/`; task procedures are indexed in [skills](../skills/README.md).
The old `.github/context/index.md` is only a compatibility pointer.
