# Skill: Bzlmod build and release

## Ubuntu 22.04 baseline

```bash
sudo bash scripts/deps/install_bazel.sh
bash scripts/release/check_bzlmod_lockfile.sh --check
bash scripts/release/ubuntu2204_baseline.sh
```

This baseline covers:

- `bazel build //cyber //:wheelos_core`
- `//cyber/message/...`
- `//cyber/python/cyber_py3/test:all`
- `//cyber/python/cyber_py3/examples:examples_smoke_test`
- `//cyber/transport/integration_test:rtps_transceiver_test`
- `//cyber/transport/rtps:rtps_test`
- `//cyber/examples/integration_test:examples_regression_tests`

## Full release artifact build

```bash
bash scripts/release/build_release_artifacts.sh
```

Outputs:

- `artifacts/release/core/` — `wheelos_core` Bazel package outputs
- `artifacts/release/pycyber/` — wheel, sdist, hashes
- `artifacts/release/manifest.txt` — build metadata summary

## pycyber-only packaging

```bash
bash scripts/release/build_and_package_pycyber.sh
```

Notes:

- A release tag must be `wheelos_core-v<module-version>` (for example,
  `wheelos_core-v1.0.0`). If `PYCYBER_VERSION` is unset and that tag is not
  checked out, the script builds a deterministic prerelease derived from the
  `MODULE.bazel` version and Git history.
- The wheel stage includes `pycyber.proto.record_pb2`, so record introspection examples remain usable after installation.
