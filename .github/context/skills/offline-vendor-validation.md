# Skill: Bazel vendor offline validation

This procedure validates Bazel 7.6.2 vendor mode without a host cache or
network access.

## Generate the vendor package

The lockfile contains registry checksums required for offline registry reads.
Do not delete it or use `--lockfile_mode=off`.

```bash
bazel mod deps --config=ci --lockfile_mode=update
bash scripts/release/check_bzlmod_lockfile.sh --check
```

Generate it with the Bazel version in `.bazelversion`.

The release script performs the lockfile refresh, vendor materialization,
local build validation, and archive creation in one step:

```bash
bash scripts/release/build_vendor_bundle.sh \
  --outdir artifacts/vendor
```

The script runs `bazel vendor --lockfile_mode=update`, which vendors repository
contents, marker files, and registry files under `vendor/bazel/_registries`.
Use `--skip-build` only when the package has already been validated separately.
After the vendor tree is created, use `--lockfile_mode=error` for all offline
commands.

## Package correctly

Include `MODULE.bazel.lock`, repository contents, marker files,
`vendor/bazel/_registries`, and `VENDOR.bazel`. Exclude `.git`, Bazel output
links, existing artifacts, and `vendor/bazel/bazel-external`.

`bazel-external` is a runtime symlink recreated by Bazel on every command.
The archive may be mounted read-only, but extraction must be into a writable
repository/vendor directory.

## Compile with the vendor package

Extract the archive into a writable directory. The archive itself may be
mounted read-only, but the extracted repository must be writable:

```bash
mkdir -p /work/wheelos-vendor
tar -xzf artifacts/vendor/wheelos_core_vendor_<timestamp>.tar.gz \
  -C /work/wheelos-vendor
cd /work/wheelos-vendor/repo
```

## Validate offline

```bash
mkdir -p /tmp/empty-repository-cache /tmp/offline-output
bazel \
  --output_user_root=/tmp/offline-output \
  build \
  --config=ci \
  --lockfile_mode=error \
  --vendor_dir=vendor/bazel \
  --repository_cache=/tmp/empty-repository-cache \
  --repository_disable_download \
  //:wheelos_core
```

Run in Ubuntu 22.04 with Docker `--network none`. Mount only the archive as
input, extract it into a new writable directory, and do not mount host Bazel
outputs or caches.

Example container invocation, using a container that contains the required
build toolchain and Bazel 7.6.2:

```bash
docker run --rm --network none \
  -v "$PWD/artifacts/vendor:/input:ro" \
  <ubuntu-22.04-build-image> \
  bash -lc '
    set -e
    rm -rf /tmp/work /tmp/offline-output /tmp/empty-repository-cache
    mkdir -p /tmp/work /tmp/offline-output /tmp/empty-repository-cache
    tar -xzf /input/wheelos_core_vendor_*.tar.gz -C /tmp/work
    cd /tmp/work/repo
    bazel --output_user_root=/tmp/offline-output build \
      --config=ci \
      --lockfile_mode=error \
      --vendor_dir=vendor/bazel \
      --repository_cache=/tmp/empty-repository-cache \
      --repository_disable_download \
      //:wheelos_core
  '
```

Pass only when the lockfile is compatible, `_registries` contains files,
`bazel-external` is absent before Bazel starts, Bazel recreates it, no network
access occurs, and `//:wheelos_core` exits successfully with its `.deb`.

Deleting the lockfile or using `--lockfile_mode=off` invalidates the test:
without checksums Bazel intentionally falls back to remote registry files.
