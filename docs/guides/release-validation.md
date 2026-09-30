# Release validation in a clean x86 container

This is the recommended release check when you want to verify what a user
will receive, rather than only checking a Bazel target on the host. It builds
an Ubuntu 22.04 x86_64 container, produces the Debian runtime package and
`pycyber` distributions, and checks that the installed tools and Python
modules can start.

## Prerequisites

- Docker with the Compose plugin
- Network access to the Ubuntu mirror, GitHub, Bazel registries, and Python
  package indexes
- A checkout with enough free space for Bazel outputs and release artifacts

The validation image installs the pinned Bazel version (`7.6.2`) from the
official Bazel GitHub release URL. The image uses the named virtual user
`wheelos` (UID/GID 1000) instead of running build commands as root.
Ubuntu 22.04 supplies `patchelf 0.14.3`, which is too old for current
auditwheel repair. The pycyber packaging script installs
`patchelf >= 0.14.5` into its build venv and selects that executable only
for repair; do not skip auditwheel to work around this version mismatch.

## Build the clean validation image

Run from the repository root:

```bash
docker build \
  --build-arg WHEELOS_UID="$(id -u)" \
  --build-arg WHEELOS_GID="$(id -g)" \
  -t wheelos-core-validation:x86_64 \
  -f docker/dev.x86_64.dockerfile .
```

The build arguments make the named `wheelos` user able to write generated
files in a bind-mounted checkout. If the checkout is intentionally owned by
UID/GID 1000, omit the two `--build-arg` options. The build prints the
installed Bazel version. It should report:

```text
bazel 7.6.2
```

## Build and validate release artifacts

Keep the container until its exit status and logs have been inspected. An
attached `docker run --rm` can forward a caller's SIGTERM and immediately
remove the evidence; separating `create`, `start`, and `wait` keeps the workload
running if the waiting client disconnects. The repository is mounted only so
that source files and generated artifacts are available on the host:

```bash
mkdir -p artifacts/release artifacts/validation
container_id="$(docker create \
  --init \
  --user wheelos \
  -e HOME=/home/wheelos \
  -e USER=wheelos \
  -v "$PWD:/workspace" \
  -w /workspace \
  --network host --ipc private --shm-size=1g \
  --ulimit memlock=-1:-1 \
  --security-opt seccomp=unconfined \
  wheelos-core-validation:x86_64 \
  bash -lc '
    set -euo pipefail
    bash scripts/release/check_bzlmod_lockfile.sh --check
    bash scripts/release/build_release_artifacts.sh --distdir /tmp/cache/
  ')"
printf '%s\n' "$container_id" > artifacts/validation/container.id
docker start "$container_id" >/dev/null
exit_code="$(docker wait "$container_id")"
docker logs "$container_id" > artifacts/validation/container.log 2>&1
docker inspect "$container_id" \
  --format 'exit={{.State.ExitCode}} oom={{.State.OOMKilled}} error={{.State.Error}} shm={{.HostConfig.ShmSize}}'
if [ "$exit_code" -ne 0 ]; then
  echo "Validation failed; inspect artifacts/validation/container.log and container $container_id" >&2
  exit "$exit_code"
fi
docker rm "$container_id" >/dev/null
rm artifacts/validation/container.id
```

The runtime matrix creates an iceoryx shared-memory segment of about 284 MB;
Docker's default 64 MB `/dev/shm` causes SIGBUS and a subsequent RouDi
timeout. Use 1 GB for the validation matrix to leave room for concurrent
segments. The development Compose service accepts `SHM_SIZE` (default `1g`);
`512m` can be selected for a constrained single workload, but is **not**
qualified for the parallel matrix. Apollo-lite defaults to 2 GB and uses
`seccomp:unconfined` for its non-privileged test container.
If the waiting shell is interrupted, use the saved `container.id` with
`docker inspect`, `docker logs`, and `docker wait` before removing that
specific container. A Docker event `kill signal=15` / exit 143 proves a
SIGTERM was sent, but does not identify the sender or prove an OOM. Keep
Valgrind as a separate gate: if intentionally skipped, report it as
**not evaluated**, never as passed.

For record, shared-memory, or io_uring examples, configure unlimited memlock:

```yaml
ulimits:
  memlock:
    soft: -1
    hard: -1
```

The validation command relaxes seccomp for `io_uring_setup` without granting
all privileges. Use `--privileged` only if the example needs additional kernel
or IPC permissions. Keep Bazel and hermetic Python packaging under the non-root
`wheelos` user: `rules_python` rejects Bazel analysis as root.

On success, the host contains:

```text
artifacts/release/core/*.deb
artifacts/release/pycyber/*.whl
artifacts/release/pycyber/*.tar.gz
artifacts/release/pycyber/SHA256SUMS
artifacts/release/manifest.txt
```

The full command runs the Ubuntu baseline first. Do not treat a
`--skip-baseline` run as a complete release validation; use it only to
separate packaging failures from a failing baseline test, and report the two
results separately.

## Verify the Debian package as a user would

This check does not install files into the container's system directories. It
extracts the package into a temporary root, loads the packaged environment,
and starts each shipped command:

```bash
DEB="$(find artifacts/release/core -maxdepth 1 -name '*.deb' -print -quit)"
docker run --rm \
  --user wheelos \
  -e HOME=/home/wheelos \
  -e USER=wheelos \
  -e DEB="$DEB" \
  -v "$PWD:/workspace:ro" \
  -w /workspace \
  wheelos-core-validation:x86_64 \
  bash -lc '
    set -euo pipefail
    rm -rf /tmp/wheelos-deb-root
    mkdir -p /tmp/wheelos-deb-root
    dpkg-deb -x "$DEB" /tmp/wheelos-deb-root
    source /tmp/wheelos-deb-root/opt/wheelos_core/setup.bash
    command -v mainboard cyber_recorder cyber_monitor cyber_launch
    mainboard --help >/tmp/mainboard-help.txt
    cyber_recorder --help >/tmp/recorder-help.txt
    cyber_monitor -h >/tmp/monitor-help.txt
    cyber_launch --help >/tmp/cyber-launch-help.txt
    test -s /tmp/cyber-launch-help.txt
  '
```

## Verify the x86 Python package

Use the wheel produced in `artifacts/release/pycyber`:

```bash
WHEEL="$(find artifacts/release/pycyber -maxdepth 1 -name '*.whl' -print -quit)"
docker run --rm \
  --user wheelos \
  -e HOME=/home/wheelos \
  -e USER=wheelos \
  -e WHEEL="$WHEEL" \
  -v "$PWD:/workspace:ro" \
  -w /workspace \
  wheelos-core-validation:x86_64 \
  python3 scripts/release/validate_pycyber.py \
    --python python3 \
    --wheel "$WHEEL"
```

This creates a temporary Python 3.10 virtual environment, installs the wheel,
and imports the public `pycyber` modules plus `record_pb2`. A successful run
prints the installed module path and the imported API symbols.

## Verify the actual io_uring reader

A successful Python record read may use the synchronous fallback. To verify
io_uring, use a non-empty **local** record file in the non-root validation
container with the runtime flags above, then run:

```bash
bazel build --config=ci //cyber/record:record_perf_reader
./bazel-bin/cyber/record/record_perf_reader \
  --file=/path/to/local.record --mode=uring_stream --max_chunks=1 \
  > /tmp/record-uring.json
python3 -c 'import json; r=json.load(open("/tmp/record-uring.json")); assert r["mode"] == "io_uring_pingpong_stream_parse" and r["chunks"] > 0 and r["messages"] > 0'
```

This mode returns nonzero if ring setup or buffer registration fails; it does
not silently substitute synchronous reads. Keep the JSON and stderr, and
compare throughput only on the same runner with an agreed baseline. If Docker
terminates the container before the command finishes, report the io_uring
gate as **not run**, not passed.

## Troubleshooting

- **Bazel downloads time out or return 404:** the container needs access to
  the configured Bazel registries and GitHub fallback URLs. A host Bazel
  cache may be mounted for speed, but it is not a substitute for validating
  the clean image.
- **`rules_python` refuses to run as root:** run the container as
  `--user wheelos`; hermetic Python intentionally rejects root.
- **The baseline fails but the package build succeeds:** keep the baseline
  failure visible and rerun packaging with `--skip-baseline` only to isolate
  the package checks. Do not report the overall release as passed.
- **The wheel is missing:** inspect the pycyber staging/build output first;
  the wheel smoke test cannot be meaningful until `artifacts/release/pycyber`
  contains a wheel.
