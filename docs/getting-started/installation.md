# Installation and setup

Choose the deployment mode that matches your goal.

## Choose a deployment path

### Source build and run

Use this path when you are working from a repository checkout and want to build and run examples directly from the source tree.

- [Source build and run](../guides/source-build-and-run.md)

### Package installation and run

Use this path when you want the installed runtime bundle and a system-level entry point.

- [Package installation and run](../guides/package-installation.md)

## Common prerequisite

Install the repository build dependencies before either path:

```bash
sudo bash scripts/deploy/build.sh
```

The repository uses Bzlmod and resolves external modules from the configured Bazel registries. Ensure network access to `https://bcr.wheelos.cn/` and the default Bazel BCR before building. In an offline or air-gapped environment, use the vendor/offline build flow instead of a standard checkout build.

## Decision guide

- Use the source build path when you are developing from the repository checkout.
- Use the package installation path when you want the installed runtime bundle.
- Use [Secondary development and integration](../guides/secondary-development.md) when you are writing a downstream Bazel project that depends on `wheelos_core`.

## Next

- [Quick start](quickstart.md)
- [Deployment options](../guides/build-and-run.md)
- [Secondary development and integration](../guides/secondary-development.md)
- [Common component example](../../examples/common_component_example/README.md)
