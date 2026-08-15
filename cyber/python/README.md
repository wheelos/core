# pycyber

`pycyber` packages the Cyber RT Python bindings from this repository.

The release flow stages Bazel-built native extensions and Python modules,
builds wheel and source distributions, repairs Linux wheels with `auditwheel`,
validates installation and imports in an isolated virtual environment, and
runs `cyber/python/cyber_py3/examples` smoke tests.

For packaging-side example verification, run:

```bash
python packaging/pycyber/verify_pycyber_examples.py
```

Release CI verifies wheel installation and import compatibility on CPython 3.10
across supported Linux architectures before publishing.

Build artifacts with:

```bash
scripts/release/build_and_package_pycyber.sh
```

Artifacts are written to `packaging/pycyber/wheelhouse/`.
