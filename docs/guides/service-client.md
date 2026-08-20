# Service and Client

This guide shows how to define a service and call it from a client process.

## Use cases

- Request/response communication
- Remote procedure calls
- Control and query patterns

## Repository references

- `cyber/python/cyber_py3/examples/service.py`
- `cyber/python/cyber_py3/examples/client.py`
- `examples/service.cc`

## Concept

A service exposes a callable API, and a client sends a request and receives a response.

## Python example flow

```bash
source scripts/env/runtime.bash
python cyber/python/cyber_py3/examples/service.py
python cyber/python/cyber_py3/examples/client.py
```

If you are running from a Bazel-built environment, use the repository entry points or the target-specific Bazel flow for your build.

## Typical service pattern

1. Initialize Cyber RT
2. Register the service
3. Wait for client requests
4. Process the request
5. Return the response

## Typical client pattern

1. Initialize Cyber RT
2. Create a client handle
3. Send a request
4. Receive and inspect the response
5. Handle errors and timeouts

## Best practices

- Keep request payloads compact
- Use explicit status or error returns
- Validate timeouts in production systems

## Next steps

- [Parameter service](parameter-service.md)
- [Record and replay](record-and-replay.md)
- [Zero-copy and sensor data](zero-copy-and-sensor-data.md)
