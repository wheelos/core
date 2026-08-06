# pycyber example

This example shows the supported Python import and Bazel target for a
`pycyber` application.

Save the following as `my_demo.py`:

```python
#!/usr/bin/env python3

from cyber.python.cyber_py3 import cyber


def main() -> None:
    cyber.init("my_demo")
    try:
        if not cyber.ok():
            raise RuntimeError("Cyber RT initialization failed")
    finally:
        cyber.shutdown()


if __name__ == "__main__":
    main()
```

Add this target to the nearest `BUILD` file:

```python
load("@rules_python//python:defs.bzl", "py_binary")

py_binary(
    name = "my_demo",
    srcs = ["my_demo.py"],
    deps = [
        "//cyber/python/cyber_py3:cyber",
    ],
)
```

Build and run the demo:

```bash
bazel build //path/to:my_demo
./bazel-bin/path/to/my_demo
```

Or:

```bash
bazel run //path/to:my_demo
```

See `cyber/python/cyber_py3/examples/` for complete publisher, subscriber,
service, timer, and record examples.
