# Parameter Service

The parameter service is used for runtime configuration and dynamic system settings.

## Use cases

- Driver configuration
- Calibration values
- Runtime tuning
- Global platform settings

## Python reference

- `cyber/python/cyber_py3/examples/parameter.py`
- `cyber/python/cyber_py3/parameter.py`

## Typical workflow

1. Start the parameter service
2. Register or update parameters
3. Query parameter values
4. Use the values in components

## Example usage

```bash
source scripts/env/runtime.bash
python cyber/python/cyber_py3/examples/parameter.py
```

## Common patterns

- Read a parameter at startup
- Refresh parameter values during runtime
- Use a parameter update mechanism for reconfiguration

## Notes

Parameter services are ideal when configuration should be centralized and updated without rebuilding the whole application.

## Next steps

- [Record and replay](record-and-replay.md)
- [Python quick start](python-quickstart.md)
- [Zero-copy and sensor data](zero-copy-and-sensor-data.md)
