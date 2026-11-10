# Installed-artifact E2E cases

These cases need only this directory, release artifacts, and one separately
built test fixture. They never import from a checkout or run `bazel-bin` tools.

```bash
bazel build //tests/e2e:fixtures
bazel test //tests/e2e:harness_test --test_output=errors
```

Stage `bazel-bin/tests/e2e/libinstalled_e2e_component.so` as
`/fixtures/libinstalled_e2e_component.so`. Build it with the same revision and
configuration as the native release. Do not copy fixture runfiles or libraries:
the fixture must load against the extracted native package's libraries.

In the runtime-only container, mount this directory at `/cases`, the release
directory at `/artifacts`, fixtures at `/fixtures`, and writable output at
`/results`, then run:

```bash
python3 /cases/installed_runtime_test.py \
  --artifacts /artifacts --fixtures /fixtures --outdir /results
```

The candidate must contain exactly one `core/*.deb`, one `pycyber/*.whl`,
`manifest.txt`, and `SHA256SUMS`. All checksums are verified before extraction.
The harness extracts the Debian package into a private user prefix, sources
its `setup.bash`, and installs the exact wheel and its dependencies in a fresh
venv. Dependency installation may use the network.
The installed `setup.bash` must export `APOLLO_ROOT_DIR` pointing at the extracted
installation prefix. The harness verifies this rather than supplying a missing
software root, and places its mainboard DAG inside that prefix, as required by
ResourceManager. Inherited software-root, configuration and asset overrides are
removed; `/apollo` and checkout paths are not required.
When a `pycyber/pycyber-*.tar.gz` source deliverable is present, it must also be
checksummed; it is independently installed in another fresh venv and all public
modules are imported, with its installed version checked against the wheel.

Full scope covers independently spawned publishers/subscribers with explicit
same-host RTPS and SHM configuration, binary protobuf payloads and exact ordered
sequences, RPC replies, native recorder capture/info/play with installed Python
record inspection and live replay validation, and native mainboard component
execution plus clean shutdown. Probe deliveries gate discovery before measured
messages are published; recorder readiness alone is not considered success.
The component fixture appends actual timer callback sequence numbers, not log
messages.

`--wheel-only` explicitly qualifies only the wheel (including ARM wheel
candidates): it requires no Debian package or native fixture, and covers RTPS
and SHM pub/sub, RPC, and Python record write/read metadata, timestamps, sequence,
and binary payload round trips. It does **not** claim native recorder,
mainboard, or Debian package acceptance.

`report.json` contains `schema_version`, `overall_status` (`passed`/`failed`),
`scope` (`full`/`wheel-only`), and `scenarios` with individual status, duration,
details or error/traceback. Exit status is nonzero on any failure. A private
`work-*` directory retains logs, extracted files, venv, recordings, and worker
results for diagnosis; reruns use unique identities and do not reuse installs.
Child processes have bounded waits and are interrupted, killed if necessary,
and reaped on both successful and failing paths.

The lightweight harness regressions are stdlib-only and can also run without
Bazel or native dependencies:

```bash
python3 tests/e2e/installed_runtime_harness_test.py
```
