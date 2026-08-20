# Deployment options

Choose the deployment path that matches your goal.

## Source build and run

Use this path when you are developing from a checkout, testing local edits, or running examples directly from the repository tree.

- [Source build and run](source-build-and-run.md)

## Package installation and run

Use this path when you want an installed runtime and want to work from the packaged native bundle instead of the source tree.

- [Package installation and run](package-installation.md)

## Common setup

Both flows start with the repository build dependencies:

```bash
sudo bash scripts/deploy/build.sh
```

## Decision guide

- Source build: you are modifying code in this repository or running examples directly from a checkout.
- Package installation: you want a stable runtime bundle, a system-level environment, or deployment-like validation.
- Downstream integration: if you are consuming `wheelos_core` from another Bazel project, use [Secondary development and integration](secondary-development.md) instead of re-reading the build commands here.

## Next

- [Installation and setup](../getting-started/installation.md)
- [Quick start](../getting-started/quickstart.md)
- [Component development](component-development.md)
