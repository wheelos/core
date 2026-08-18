# Vendor offline validation report

**Result: PASS**

Validated on Ubuntu 22.04 with Bazel 7.6.2 using the generated vendor archive
and Docker `--network none`. The archive was extracted into a writable
directory, with an empty repository cache and a fresh Bazel output root.

The archive was generated with:

```bash
bash scripts/release/build_vendor_bundle.sh --outdir artifacts/vendor
```

```bash
bazel --output_user_root=/tmp/out build \
  --config=ci \
  --lockfile_mode=error \
  --vendor_dir=vendor/bazel \
  --repository_cache=/tmp/cache \
  --repository_disable_download \
  //:wheelos_core
```

The build completed successfully with 3,394 actions and produced
`bazel-bin/wheelos_core_1.0.3_all.deb`. The archive contains the Bazel 7.6.2
lockfile, vendored repositories, marker files, and `_registries`; it does not
contain `bazel-external`, which Bazel creates at runtime.

The release workflow must first run `bazel mod deps --lockfile_mode=update` and
`bazel vendor --lockfile_mode=update` while online. Offline validation and
builds must retain the lockfile and use `--lockfile_mode=error`; deleting the
lockfile or using `--lockfile_mode=off` intentionally causes registry fallback.
