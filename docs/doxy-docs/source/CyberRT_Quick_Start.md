# Build and run a component

Components are dynamically loaded modules. A component declares its input
readers, implements `Init()` and `Proc(...)`, and is registered with
`CYBER_REGISTER_COMPONENT`.

This guide uses `examples/common_component_example`. Run commands from the
repository root after building the example.

## 1. Implement the component

```cpp
#include <memory>

#include "cyber/component/component.h"
#include "examples/proto/examples.pb.h"

using apollo::cyber::Component;
using apollo::cyber::examples::proto::Driver;

class CommonComponentSample : public Component<Driver, Driver> {
 public:
  bool Init() override;
  bool Proc(const std::shared_ptr<Driver>& first,
            const std::shared_ptr<Driver>& second) override;
};

CYBER_REGISTER_COMPONENT(CommonComponentSample)
```

The public C++ namespace remains `apollo::cyber` for source compatibility.
The repository and package identity are `wheelos_core`.

## 2. Declare the Bazel targets

The example builds a shared library and two writer binaries:

```python
cc_binary(
    name = "libcommon_component_example.so",
    linkshared = True,
    linkstatic = False,
    deps = [":common_component_example_lib"],
)

cc_library(
    name = "common_component_example_lib",
    srcs = ["common_component_example.cc"],
    hdrs = ["common_component_example.h"],
    deps = [
        "//cyber",
        "//examples/proto:examples_cc_proto",
    ],
)
```

Build it with:

```bash
bazel build //examples/common_component_example/...
```

## 3. Configure the DAG

The `readers` entries must match the component arity. This two-input component
therefore has two reader entries:

```protobuf
module_config {
  module_library: "bazel-bin/examples/common_component_example/libcommon_component_example.so"
  components {
    class_name: "CommonComponentSample"
    config {
      name: "common"
      readers { channel: "/wheelos/examples/prediction" }
      readers { channel: "/wheelos/examples/test" }
    }
  }
}
```

The repository example stores this configuration in
`examples/common_component_example/common.dag`. A launch file can reference
the same DAG:

```xml
<cyber>
  <component>
    <name>common</name>
    <dag_conf>examples/common_component_example/common.dag</dag_conf>
    <process_name>common</process_name>
  </component>
</cyber>
```

## 4. Run the component

For a source checkout, load the repository runtime environment:

```bash
source scripts/env/runtime.bash
export GLOG_alsologtostderr=1
```

Start the component with either the launch file or `mainboard`:

```bash
cyber_launch start examples/common_component_example/common.launch
# or:
mainboard -d examples/common_component_example/common.dag
```

In two additional terminals, build and run the writers:

```bash
bazel run //examples/common_component_example:channel_test_writer
bazel run //examples/common_component_example:channel_prediction_writer
```

The component should log each pair of received `Driver` messages. The timer
variant in `examples/timer_component_example` demonstrates
`TimerComponent` and does not require input readers.

## Common mistakes

- A component with `Component<M0, M1>` needs at least two `readers`.
- The shared-library path in the DAG must point to the current `bazel-bin`
  output, not an installation from another checkout.
- Run `mainboard` or `cyber_launch` from a shell with the matching runtime
  environment.
