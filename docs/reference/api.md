# API reference

The repository uses generated API documentation for the code-level reference and task-oriented docs for the operational workflow.

## Supported public surface

External Bazel consumers should use `@core//cyber` for the application
convenience API, or the explicitly public targets below for narrower
dependencies. `//cyber:cyber_core` is the runtime implementation and shared
library packaging target, not an external entry point.

| Responsibility | Public C++ targets |
| --- | --- |
| Startup and process state | `//cyber`, `//cyber:init`, `//cyber:state` |
| Pub/sub | `//cyber/node`, `:reader`, `:reader_base`, `:writer`, `:writer_base` |
| Components | `//cyber/component`, `:component_base`, `:timer_component` |
| RPC | `//cyber/service`, `:client`, `:client_base`, `:service_base` |
| Parameters | `//cyber/parameter`, `:parameter_client`, `:parameter_server` |
| Tasks and time | `//cyber/task`, `//cyber/timer`, `//cyber/time`, `//cyber/time:clock`, `:duration`, `:rate` |
| Record files | `//cyber/record`, `:record_reader`, `:record_writer`, `:record_viewer`, `:record_base`, `:record_message` |
| Message adapters | `//cyber/message:raw_message`, `:flatbuffers_message` |
| GPU channels | `//cyber/gpu:gpu_reader`, `:gpu_writer` |
| Common utilities | `//cyber/common:file`, `:log`, `:macros`, `:resource_manager` |

Within each table cell, a `:name` uses the preceding full target's package.
`cyber/node/node.h` declares `CreateNode`; applications can depend on
`//cyber/node` and `//cyber:init` without the umbrella `//cyber` target.
GPU factories and types retain their existing `apollo::cyber::transport`
namespace; only their source and Bazel ownership changes.

The following extensions are deliberately public for existing integrations:

- `//cyber/record:header_builder` and `:record_file_reader` support record
  processors.
- `//cyber/tools/cyber_recorder:recorder` supports embedding the recorder;
  its filter and policy implementation targets remain internal.
- `//cyber:cyber_conf` supplies runtime configuration files.
- The `clock`, `component_conf`, `dag_conf`, `parameter`, `qos_profile`,
  `role_attributes`, `record`, and `run_mode_conf` schemas under
  `//cyber/proto` expose their `_proto`, `_cc_proto`, and `_py_pb2` targets.
  Other runtime schemas are implementation support, not independent APIs.

Python consumers use `//cyber/python/cyber_py3:cyber`, `:cyber_time`,
`:cyber_timer`, `:parameter`, and `:record`. The native binding module and
Python-specific shutdown hooks are internal. Runtime command targets under
`//cyber/mainboard` and `//cyber/tools/<command>` remain public executables.

### Enforcement and SDK headers

All `cyber/` packages default to the repository-only `//:internal` package
group. Public targets opt in explicitly. Scheduler policies, discovery
managers, transport implementations, class-loader internals, and native
bindings cannot be direct dependencies of external applications. The public
convenience target links the runtime through `implementation_deps`, so it
does not re-export every implementation header.

The `targets` list on `//:wheelos_cyber_headers` in the root BUILD file is
the explicit C++ SDK header-root inventory. `//:wheelos_cyber_public_headers`
selects only those roots. The complete SDK also contains their required
transitive headers, including generated protobuf imports and template
implementation headers. Those support files are required for compilation;
their presence does **not** make their implementation classes stable APIs.
Do not include support headers directly or depend on their internal targets.
Adding a dependency to `cyber_core` alone no longer expands the SDK.

The SDK installs `wheelos_cyber_headers.public.txt` and
`wheelos_cyber_headers.all.txt` under `/usr/local/share/wheelos_core`.
The former lists supported header roots; the latter also includes the
compilation support closure.

`//tests/public_api:sdk_contract_test` checks actual Bazel visibility, compiles
RPC headers independently and GPU factories through public dependencies,
and verifies that every SDK-local include is present, including the recorder
extension and the public generated proto headers. The
`//tests/public_api:node_public_api_test` target links and runs node creation
and writer creation using only the narrow public dependencies. The consumer
package is intentionally excluded from `//:internal`.

### Source migration

Update GPU includes from `cyber/transport/nvsci/gpu_reader.h` and
`gpu_writer.h` to `cyber/gpu/gpu_reader.h` and `gpu_writer.h`, and dependencies
from `//cyber/transport/nvsci:gpu_reader` / `:gpu_writer` to
`//cyber/gpu:gpu_reader` / `:gpu_writer`. Repository consumers migrate
atomically; no forwarding aliases are retained.

`IntraReader` and `IntraWriter` are simulation-mode Node adapters under
`cyber/node/internal`, not public alternatives to `Node::CreateReader` and
`Node::CreateWriter`. Applications should use the Node APIs.

## Generated API docs

The generated documentation under `docs/doxy-docs` describes code-level
details, including internal implementation. It does not expand the supported
public surface defined above:

- [Documentation index](../doxy-docs/source/index.md)
- [C++ API](../doxy-docs/source/api/cppapi_index.rst)
- [Python API](../doxy-docs/source/api/pythonapi_index.rst)
- [C++ API guide](../doxy-docs/source/cpp-api.md)
- [Python API guide](../doxy-docs/source/python-api.md)
- [Terms](../doxy-docs/source/terms.md)

## Usage model

The generated docs are best used for:

- C++ API lookup
- Python API lookup
- scheduler and deployment concepts
- framework terminology and deeper implementation details

Use the task-oriented docs under `docs/getting-started/` and `docs/guides/` for installation and day-to-day usage.

## Related documentation

- [Documentation index](../README.md)
- [Installation guide](../getting-started/installation.md)
- [Quick start](../getting-started/quickstart.md)
- [Deployment options](../guides/build-and-run.md)
