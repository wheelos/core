# Python Quick Start

This guide focuses on using Cyber RT from Python.

## Repository references

- `cyber/python/README.md`
- `cyber/python/cyber_py3/examples/*.py`
- `packaging/pycyber/README.md`

## Minimal example

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

## Build and run

```bash
source scripts/env/runtime.bash
bazel build //path/to:my_demo
bazel run //path/to:my_demo
```

## Python examples in this repo

The repository includes examples for:

- talker
- listener
- service
- client
- parameter
- record
- timer
- time

Examples directory:

- `cyber/python/cyber_py3/examples`

## Example commands

```bash
python cyber/python/cyber_py3/examples/talker.py
python cyber/python/cyber_py3/examples/listener.py
python cyber/python/cyber_py3/examples/service.py
python cyber/python/cyber_py3/examples/client.py
python cyber/python/cyber_py3/examples/parameter.py
python cyber/python/cyber_py3/examples/record.py
```

## Best practices

- Initialize once per process
- Shut down cleanly
- Use explicit logging for debugging
- Prefer the repository examples as templates

## Next steps

- [Publish/Subscribe Basics](pubsub-basics.md)
- [Service and client](service-client.md)
- [Parameter service](parameter-service.md)
- [Zero-copy and sensor data](zero-copy-and-sensor-data.md)
