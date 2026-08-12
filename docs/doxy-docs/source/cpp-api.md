# C++ API guide

This guide summarizes the application-facing C++ API. The
[generated C++ reference](api/cppapi_index.rst) is authoritative for
signatures and members.

The public namespace is `apollo::cyber`; the package and Bazel module are
`wheelos_core`.

## Initialize the runtime

Every native process initializes Cyber RT before creating a node:

```cpp
#include "cyber/cyber.h"

int main(int argc, char** argv) {
  apollo::cyber::Init(argv[0]);
  auto node = apollo::cyber::CreateNode("example");
  apollo::cyber::WaitForShutdown();
  return 0;
}
```

Use `OK()` for a loop that should stop on shutdown. `WaitForShutdown()` blocks
until the runtime receives a shutdown request.

## Publish and subscribe

`Node::CreateWriter<MessageT>` creates a typed writer. A reader invokes a
callback when a message arrives:

```cpp
#include "cyber/cyber.h"
#include "examples/proto/examples.pb.h"
#include "cyber/time/rate.h"

using apollo::cyber::Rate;
using apollo::cyber::examples::proto::Chatter;

int main(int argc, char** argv) {
  apollo::cyber::Init(argv[0]);
  auto node = apollo::cyber::CreateNode("talker");
  auto writer = node->CreateWriter<Chatter>("/wheelos/examples/chatter");
  Rate rate(1.0);
  uint64_t sequence = 0;
  while (apollo::cyber::OK()) {
    auto message = std::make_shared<Chatter>();
    message->set_seq(sequence++);
    message->set_content("hello from wheelos_core");
    writer->Write(message);
    rate.Sleep();
  }
}
```

```cpp
void OnMessage(
    const std::shared_ptr<apollo::cyber::examples::proto::Chatter>& message) {
  AINFO << "received sequence " << message->seq();
}

int main(int argc, char** argv) {
  apollo::cyber::Init(argv[0]);
  auto node = apollo::cyber::CreateNode("listener");
  node->CreateReader<apollo::cyber::examples::proto::Chatter>(
      "/wheelos/examples/chatter", OnMessage);
  apollo::cyber::WaitForShutdown();
}
```

Build the repository examples with:

```bash
bazel build //examples/...
```

Run the binaries from separate terminals after sourcing
`scripts/env/runtime.bash`:

```bash
./bazel-bin/examples/talker
./bazel-bin/examples/listener
```

## Services and clients

Services use protobuf request and response types. The callback fills the
response; clients call `SendRequest`:

```cpp
using apollo::cyber::examples::proto::Driver;

auto node = apollo::cyber::CreateNode("service_demo");
auto server = node->CreateService<Driver, Driver>(
    "/wheelos/examples/echo",
    [](const std::shared_ptr<Driver>& request,
       std::shared_ptr<Driver>& response) {
      response->CopyFrom(*request);
    });
auto client = node->CreateClient<Driver, Driver>("/wheelos/examples/echo");

auto request = std::make_shared<Driver>();
request->set_msg_id(1);
auto response = client->SendRequest(request);
```

The complete executable is in `examples/service.cc`:

```bash
bazel run //examples:service
```

## Parameters

`ParameterServer` owns parameters for a node. `ParameterClient` accesses a
server exposed by another node:

```cpp
#include "cyber/parameter/parameter_client.h"
#include "cyber/parameter/parameter_server.h"

auto server = std::make_shared<apollo::cyber::ParameterServer>(node);
server->SetParameter(apollo::cyber::Parameter("threshold", 10));

auto client = std::make_shared<apollo::cyber::ParameterClient>(
    node, "parameter_node");
apollo::cyber::Parameter value;
client->GetParameter("threshold", &value);
AINFO << value.AsInt64();
```

## Logging

Include `cyber/cyber.h` and use the standard macros:

```cpp
ADEBUG << "verbose detail";
AINFO << "normal progress";
AWARN << "recoverable problem";
AERROR << "operation failed";
```

For a short interactive session:

```bash
export GLOG_alsologtostderr=1
```

The installed runtime writes logs below
`${XDG_STATE_HOME:-$HOME/.local/state}/wheelos_core/log` by default. Set
`GLOG_log_dir` explicitly for a service manager.

## Components and timers

Use a `Component<M0, ...>` when a class should be loaded from a shared
library and receive messages through DAG-configured readers. Use
`TimerComponent` for periodic work. The [component tutorial](../../getting-started/quickstart.md)
contains a complete BUILD, DAG, launch, and runtime example.

The timer example can be built and run with:

```bash
bazel build //examples/timer_component_example/...
source scripts/env/runtime.bash
mainboard -d examples/timer_component_example/timer.dag
```

## Time, duration, and rate

```cpp
auto now = apollo::cyber::Time::Now();
uint64_t nanoseconds = now.ToNanosecond();
double seconds = now.ToSecond();

apollo::cyber::Duration pause(1000000);  // one millisecond
pause.Sleep();

apollo::cyber::Rate rate(10.0);  // ten cycles per second
rate.Sleep();
```

`Timer` runs a callback periodically or once:

```cpp
apollo::cyber::Timer timer(
    100, [] { AINFO << "timer tick"; }, false);
timer.Start();
```

## Record files

`RecordWriter` writes raw or serialized messages; `RecordReader` reads them
back. The repository example writes and reads `test.record`:

```bash
bazel run //examples:record
```

The central native operations are:

```cpp
apollo::cyber::record::RecordWriter writer;
writer.Open("example.record");
writer.WriteChannel("/wheelos/examples/raw", "raw", "");
writer.WriteMessage("/wheelos/examples/raw", raw_message, timestamp);
writer.Close();

apollo::cyber::record::RecordReader reader("example.record");
apollo::cyber::record::RecordMessage message;
while (reader.ReadMessage(&message)) {
  AINFO << message.channel_name << " at " << message.time;
}
```

## API map

| Task | Primary API |
| --- | --- |
| Runtime lifecycle | `Init`, `OK`, `WaitForShutdown` |
| Graph endpoint | `Node` |
| Pub/sub | `Writer`, `Reader` |
| RPC | `Service`, `Client` |
| Parameters | `Parameter`, `ParameterServer`, `ParameterClient` |
| Periodic execution | `Timer`, `TimerComponent` |
| Time | `Time`, `Duration`, `Rate` |
| Capture/playback | `RecordReader`, `RecordWriter` |

See the generated [C++ API reference](api/cppapi_index.rst) for all public
classes and methods.
