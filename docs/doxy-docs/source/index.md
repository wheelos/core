# wheelos_core documentation

`wheelos_core` is a Bazel-first Cyber RT middleware foundation. It provides
publish/subscribe channels, service and client RPC, component loading,
coroutine scheduling, topology discovery, shared-memory and RTPS transport,
record I/O, native tools, and Python bindings.

## Start here

1. [Install and use wheelos_core](Wheelos_Core_Installation_and_Usage.md)
2. [Build a component](CyberRT_Quick_Start.md)
3. [Use the C++ API](CyberRT_API_for_Developers.md)
4. [Use the Python API](CyberRT_Python_API.md)

## Guides

- [Developer tools](CyberRT_Developer_Tools.md)
- [Scheduling and deployment](CyberRT_Scheduling_Deployment.md)
- [Terminology](CyberRT_Terms.md)
- [Release baseline](WheelOS_Core_Baseline.md)

## API reference

The API reference is generated from the current C++ and Python sources with
Doxygen and rendered by Sphinx:

- [C++ API](api/cppapi_index.rst)
- [Python API](api/pythonapi_index.rst)

.. toctree::
   :maxdepth: 2
   :caption: Guides

   Wheelos_Core_Installation_and_Usage
   CyberRT_Quick_Start
   CyberRT_API_for_Developers
   CyberRT_Python_API
   CyberRT_Developer_Tools
   CyberRT_Scheduling_Deployment
   CyberRT_Terms
   WheelOS_Core_Baseline

## Build the documentation

From `docs/doxy-docs`:

```bash
doxygen Doxyfile
sphinx-build -b html source build/html
```

Read the Docs uses the same Sphinx entry point through the repository
`.readthedocs.yaml` configuration.
