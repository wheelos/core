# Topology and transport

## Before you begin

The runtime is organized around topology discovery, node/service registration, and transport.

## Relevant modules

- `cyber/service_discovery`
- `cyber/transport`
- `cyber/transport/rtps`
- `cyber/node`
- `cyber/service`

The build graph exposes these through `//cyber:cyber` and `//cyber:cyber_core`.

## Runtime model

The middleware is message-oriented:

- nodes publish and subscribe on channels
- services and clients communicate over RPC-style interfaces
- components process incoming messages in the runtime scheduler

## Transport stack

The repo references transport layers for:

- INTRA
- SHM
- RTPS

This is reflected in the build targets under `//cyber/transport` and `//cyber/transport/rtps`.

## Verify

This map is a runtime-level overview; use generated API docs for advanced implementation detail.

## Next

- [Deployment options](build-and-run.md)
- [Component development](component-development.md)
- [Generated API index](../doxy-docs/source/index.md)
