# Record and Replay

This guide explains how to record message data and replay it later for debugging, validation, and analysis.

## Use cases

- Sensor data capture
- System debugging
- Offline testing
- Data analysis and reproduction

## Repository references

- `cyber/python/cyber_py3/examples/record.py`
- `cyber/python/cyber_py3/examples/record_channel_info.py`
- `cyber/python/cyber_py3/examples/record_trans.py`
- `cyber/tools/cyber_recorder`

## Record workflow

Record data from channels using the recorder tool or a Python example.

```bash
source scripts/env/runtime.bash
cyber_recorder record --help
```

You can then configure which channels to capture and where to store the recorded file.

## Replay workflow

Replay a recorded file to validate message flow or reproduce an earlier system state.

```bash
cyber_recorder play --help
```

Use channel selection and replay configuration to focus on specific topics.

## Best practices

- Record only required channels
- Use channel filters for large sensor streams
- Keep replay files organized by dataset and timestamp
- Validate file integrity before using replay for regression testing

## Notes

This is especially useful for camera, lidar, and other high-volume sensor workflows.

## Next steps

- [Zero-copy and sensor data](zero-copy-and-sensor-data.md)
- [Python quick start](python-quickstart.md)
- [Component development](component-development.md)
