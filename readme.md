## Core
Core is a fork of Apollo cyber, a publish-subscribe system used as middleware for autonomous driving.

## Benchmark
Latency of publish and subscribe messages.

## How to build

1. Deploy build env.

```bash
sudo bash scripts/deploy/build.sh
```

2. Run the build script.

```bash
bash scripts/build.sh
```

For the Ubuntu 22.04 baseline used by CI and release validation, run:

```bash
bash scripts/release/ubuntu2204_baseline.sh
```

To verify or refresh the Bzlmod lockfile:

```bash
bash scripts/release/check_bzlmod_lockfile.sh --check
bash scripts/release/check_bzlmod_lockfile.sh --update
```

For a narrower `pycyber`-focused build/test loop, use the existing Bazel targets directly:

```bash
bazel test --color=no --curses=no \
  //cyber/python/cyber_py3/test:all \
  //cyber/python/cyber_py3/examples:examples_smoke_test
```

## Example

1. Set Environment Variable
   ```bash
   source scripts/env/runtime.bash
   ```

2. Run the Publisher and Subscriber
    - In the first terminal, run `./bazel-bin/cyber/examples/listener` to start the subscriber program.
    - In the second terminal, run `./bazel-bin/cyber/examples/talker` to start the publisher program.

3. Check the log in `data/log`

## Consuming wheelos_core from Bazel

Use the published module name in reusable BUILD files:

```starlark
bazel_dep(name = "wheelos_core", version = "1.0.0")
```

```starlark
cc_binary(
    name = "consumer",
    srcs = ["consumer.cc"],
    deps = ["@wheelos_core//cyber:cyber"],
)
```

wheelos_core requires C++17. Downstream workspaces must configure a C++17
toolchain (for example, `build --cxxopt=-std=c++17` and
`build --host_cxxopt=-std=c++17` in `.bazelrc`).

Use `@wheelos_core//cyber:runtime_tools` to build the supported runtime
executables, or `bazel run @wheelos_core//cyber/tools/cyber_launch:cyber_launch`
to run a tool through Bazel runfiles. See
`data/context/wheelos-core-sdk-contract.md` for the complete API and release
contract.

The Debian artifact is a runtime bundle, not an installed C++ SDK; use the
Bazel source-SDK interface above for C++ development.

## Runtime bundle

`//:wheelos_core` produces a self-contained native runtime bundle. After
installing its Debian artifact, initialize an interactive shell with:

```bash
source /opt/wheelos_core/setup.bash
cyber_launch --help
```

The bundle contains `mainboard`, `cyber_recorder`, `cyber_monitor`,
`cyber_launch`, runtime libraries, and Cyber configuration resources. Python
applications and Python command-line tools are distributed through the matching
`pycyber` wheel, not through `PYTHONPATH` pointing at Bazel output directories.

## Build the pycyber wheel

Recommended: use the included one-step packaging script which stages, builds, repairs and validates the wheel. Always run packaging inside an isolated Python environment (venv) or a clean container to avoid dependency conflicts.

For a full release-oriented artifact build that also collects the `wheelos_core` package outputs, run:

```bash
bash scripts/release/build_release_artifacts.sh
```

Quick start (recommended)

1. Create and activate a virtual environment (venv):

```bash
python3.11 -m venv .packaging-venv
source .packaging-venv/bin/activate
python -m pip install --upgrade pip
pip install build auditwheel twine setuptools_scm
# On Linux, also install patchelf
sudo apt-get update && sudo apt-get install -y patchelf
```

2. Run the one-step packaging script. The script uses `PYCYBER_VERSION` if
   set; otherwise an exact `wheelos_core-v<module-version>` tag produces the
   release version and other commits produce a deterministic prerelease:

```bash
# Preferred: use tag-derived version (make and push a tag first)
./scripts/release/build_and_package_pycyber.sh

# Or override version explicitly:
PYCYBER_VERSION=0.0.7 ./scripts/release/build_and_package_pycyber.sh
```

Artifacts and checks:

- Artifacts are written to `packaging/pycyber/wheelhouse/`.
- `packaging/pycyber/{staging,build,dist,wheelhouse}` are generated packaging outputs; keep the checked-in sources under `packaging/pycyber/` and `scripts/release/` as the canonical release inputs.
- The script runs `twine check` and the repository's `validate_pycyber.py` smoke test.
- A `SHA256SUMS` file is created in the wheelhouse; upload only `*.whl` and `*.tar.gz` to PyPI.
- Official release uploads currently publish one sdist plus Linux wheels for `x86_64` and `aarch64`, built with CPython 3.11 as `cp311` platform wheels.

Script flags:

- `--skip-auditwheel` : Skip `auditwheel` repair (useful on non-Linux machines).
- `--skip-validate` : Skip running the smoke validation.
- `--skip-sdist` : Only build wheel, skip sdist.
- `--skip-build` : Skip staging (`build_pycyber.py`), useful if staging already exists.
- `--outdir DIR` : Write artifacts to a custom directory.

Manual (step-by-step) packaging

If you prefer to run the steps manually, follow these commands (run inside a clean venv / container):

1) Stage the package from Bazel outputs:

```bash
python scripts/release/build_pycyber.py
```

2) Build sdist + wheel:

```bash
python -m build --sdist --wheel --outdir packaging/pycyber/dist packaging/pycyber
```

3) Repair Linux wheels (auditwheel):

```bash
auditwheel repair packaging/pycyber/dist/*.whl -w packaging/pycyber/wheelhouse
```

4) Check metadata and run validation:

```bash
python -m twine check packaging/pycyber/wheelhouse/*.whl packaging/pycyber/wheelhouse/*.tar.gz
python scripts/release/validate_pycyber.py --wheel packaging/pycyber/wheelhouse/*.whl
```

5) Upload to TestPyPI (recommended first):

```bash
TWINE_USERNAME=__token__ TWINE_PASSWORD=$TEST_PYPI_TOKEN \
  python -m twine upload --repository testpypi packaging/pycyber/wheelhouse/*.whl packaging/pycyber/wheelhouse/*.tar.gz

# Test install from TestPyPI in a fresh venv:
pip install --index-url https://test.pypi.org/simple/ --no-deps pycyber==0.0.7
```

6) Final upload to PyPI:

```bash
TWINE_USERNAME=__token__ TWINE_PASSWORD=$PYPI_TOKEN \
  python -m twine upload packaging/pycyber/wheelhouse/*.whl packaging/pycyber/wheelhouse/*.tar.gz
```

Versioning

- `pyproject.toml` is configured to use `setuptools_scm` with the tag pattern `^wheelos_core-v(?P<version>.+)$`.
- Preferred release flow: create an annotated tag on the release commit and push it:

```bash
git tag -a wheelos_core-v1.0.0 -m "Release 1.0.0"
git push origin --tags
```

- The one-step script will extract the version from the tag automatically. If needed, override with `PYCYBER_VERSION`.

Troubleshooting

- pip dependency conflicts (for example: `streamlit` vs `protobuf`) occur when build tools are installed into the global Python environment. Always use a clean virtualenv (`venv`) or container for packaging.
- For reproducible manylinux wheels and multi-Python builds, use `cibuildwheel` or the official manylinux Docker images: https://github.com/pypa/manylinux
- If `auditwheel` cannot patch RPATH because `patchelf` is missing, install `patchelf` (Debian/Ubuntu: `apt-get install -y patchelf`) or run the packaging inside an appropriate Linux build environment.

Security and best practices

- Use PyPI API tokens (username `__token__`) with minimal scope; store tokens as secrets in CI.
- Upload to TestPyPI first to verify installation before publishing to the real PyPI.
- Tag releases and attach built artifacts and `SHA256SUMS` to GitHub Releases.
- Optionally GPG-sign artifacts for extra authenticity.

CI

- `.github/workflows/c-cpp.yml` runs the Ubuntu 22.04 Bzlmod baseline entrypoint.
- `.github/workflows/release-pycyber.yml` is triggered by tags `wheelos_core-v*`. It checks out the full history (`fetch-depth: 0`) so `setuptools_scm` can compute versions correctly, builds release artifacts on `x86_64` and `aarch64`, then verifies wheel install/import on CPython 3.11 before publish.
