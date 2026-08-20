# wheelos_core API and implementation reference

This section is the generated reference for the repository's implementation and
APIs. It is intended for developers who already understand the project and are
looking for code-level details, component wiring, scheduling concepts, and
framework terminology.

For first-time setup and end-user workflow, start with the repository's task-
orientated documentation instead:

- [Documentation index](../../README.md)
- [Installation and setup](../../getting-started/installation.md)
- [Quick start](../../getting-started/quickstart.md)
- [Deployment options](../../guides/build-and-run.md)

## Primary purpose of this section

This API reference is generated from the current C++ and Python sources with
Doxygen and rendered by Sphinx. Use it when you need implementation detail,
public APIs, or framework concepts, not when you are onboarding to the project.

## API entry points

- [C++ API](api/cppapi_index.rst)
- [Python API](api/pythonapi_index.rst)

## Reference pages in this section

- [C++ API](cpp-api.md)
- [Python API](python-api.md)
- [Terminology](terms.md)

.. toctree::
   :maxdepth: 2
   :caption: API and implementation reference

   cpp-api
   python-api
   terms

## Build the documentation

From `docs/doxy-docs`:

```bash
doxygen Doxyfile
sphinx-build -b html source build/html
```

Read the Docs uses the same Sphinx entry point through the repository
`.readthedocs.yaml` configuration.
