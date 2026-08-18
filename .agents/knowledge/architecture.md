# Architecture

## When

Read this when changing Cyber RT initialization, nodes, components, scheduling,
discovery, or transport.

## Rules / Facts

- `cyber/cyber.h` is the public entry point; `cyber/init.cc` owns framework
  startup and shutdown.
- `Node` exposes pub/sub through the channel API and RPC through the service
  API; their implementations are `NodeChannelImpl` and `NodeServiceImpl`.
- `mainboard` reads DAG files and assembles modules through `ModuleController`,
  the class loader, and component registration.
- The transport layer supports INTRA, SHM, RTPS, and HYBRID; the transport
  configuration selects the default.
- Service discovery manages topology separately from data transport.
- The scheduler turns component reader work into tasks; component authors
  implement `Init` and `Proc` rather than taking over framework `Process`.

## Sources

- `cyber/cyber.h`
- `cyber/init.cc`
- `cyber/node/`
- `cyber/mainboard/`
- `cyber/transport/`
- `cyber/service_discovery/`
- `cyber/scheduler/`
