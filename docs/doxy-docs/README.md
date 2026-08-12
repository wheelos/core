# Doxygen and Sphinx reference docs

This directory contains the generated API and implementation reference for
`wheelos_core`.

It is not the primary entry point for new users. For installation and day-to-day
usage, prefer the operational documentation under:

- [docs/README.md](../README.md)
- [docs/getting-started/installation.md](../getting-started/installation.md)
- [docs/getting-started/quickstart.md](../getting-started/quickstart.md)
- [docs/guides/build-and-run.md](../guides/build-and-run.md)

The reference content here is intended for:

- API lookup
- framework concepts
- component authoring
- implementation details
- release and scheduling context

## Build locally

From this directory:

```bash
python3 -m pip install -r requirements.txt
make html
```

The generated site is written to `build/html`.
