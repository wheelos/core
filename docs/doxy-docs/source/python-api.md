# Python API

`pycyber` provides Python bindings for channels, services, record files,
time, timers, and topology queries. The [generated Python reference](api/pythonapi_index.rst)
is authoritative for the installed version.

## Install

Install the wheel produced with the matching runtime artifact:

```bash
bash scripts/release/build_release_artifacts.sh
python3 -m pip install artifacts/release/pycyber/pycyber-*.whl
```

For a Bazel Python target, depend on
`@wheelos_core//cyber/python/cyber_py3:cyber` instead of modifying
`PYTHONPATH`.

## Lifecycle and pub/sub

```python
from cyber.python.cyber_py3 import cyber
from my_messages_pb2 import Chatter


def on_message(message):
    print(message.seq, message.content)


cyber.init("python_listener")
node = cyber.Node("listener")
reader = node.create_reader(
    "/wheelos/examples/chatter",
    Chatter,
    on_message,
)
writer = node.create_writer(
    "/wheelos/examples/chatter",
    Chatter,
)

message = Chatter(seq=1, content="hello")
writer.write(message)
node.spin()
cyber.shutdown()
```

`Node.create_reader` deserializes protobuf messages before invoking the
callback. Use `create_rawdata_reader` when the application needs raw bytes.
Close nodes and resources explicitly, or use their context-manager forms.

## Services

```python
from cyber.python.cyber_py3 import cyber
from my_messages_pb2 import Driver


def handle(request):
    return Driver(
        msg_id=request.msg_id + 1,
        timestamp=request.timestamp,
    )


cyber.init("python_service")
node = cyber.Node("service")
service = node.create_service(
    "/wheelos/examples/echo",
    Driver,
    Driver,
    handle,
)
client = node.create_client(
    "/wheelos/examples/echo",
    Driver,
    Driver,
)
response = client.send_request(Driver(msg_id=1))
cyber.shutdown()
```

## Record files

```python
from cyber.python.cyber_py3 import record


with record.RecordWriter() as writer:
    writer.open("example.record")
    writer.write_channel("/wheelos/examples/raw", "raw", "")
    writer.write_message("/wheelos/examples/raw", b"payload", 1)

with record.RecordReader("example.record") as reader:
    for topic, data, data_type, timestamp in reader.read_messages():
        print(topic, data_type, timestamp, data)
```

`RecordReader.read_messages()` yields `(topic, data, data_type, timestamp)`.
Use `get_messagenumber`, `get_messagetype`, `get_protodesc`, and
`get_channellist` for metadata.

## Time and rate

```python
from cyber.python.cyber_py3.cyber_time import Duration, Rate, Time

started = Time.now()
Rate(10.0).sleep()
elapsed = Time.now() - started
print(elapsed.to_sec())
Duration(1_000_000).sleep()
```

`Time` and `Duration` use nanoseconds internally. `Rate(float)` accepts a
frequency in cycles per second.

## Topology utilities

The Python bindings expose discovery helpers for diagnostics:

```python
from cyber.python.cyber_py3.cyber import ChannelUtils, NodeUtils, ServiceUtils

channels = ChannelUtils.get_channels()
nodes = NodeUtils.get_nodes()
services = ServiceUtils.get_services()
```

Use the native `cyber_monitor` for interactive inspection and the generated
[Python API reference](api/pythonapi_index.rst) for the full method list.
