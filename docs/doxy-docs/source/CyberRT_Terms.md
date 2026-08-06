# Cyber RT terms

## Component

A dynamically loaded application module. A component implements `Init()` and
`Proc(...)`, declares its input readers, and is registered with
`CYBER_REGISTER_COMPONENT`.

## Node

The application-facing object that owns publishers, subscribers, services,
clients, timers, and parameters.

## Channel

A named publish/subscribe data path. Writers publish messages and readers
receive messages through the configured transport.

## Writer and reader

A writer publishes typed or raw messages to a channel. A reader subscribes to
a channel and invokes its callback or schedules a component task.

## Service and client

A service handles request/response RPC calls. A client sends requests and
waits for responses. Service discovery is separate from message transport.

## Parameter

A key/value API implemented through the service/client mechanism. Parameters
can be read and updated by nodes that share the same discovery domain.

## Task and coroutine

A task is a schedulable unit of work. Cyber RT uses cooperative, stackful
coroutines; blocking work can occupy its assigned Processor and delay other
tasks.

## Component DAG

A protobuf configuration that names shared libraries, registered classes,
reader channels, and timer components. `mainboard` loads the DAG.

## Record

An on-disk stream of channel messages used for capture, inspection, and
playback. The native recorder tools and Python record APIs provide access to
record files.

## Transport

The runtime can select in-process, shared-memory, RTPS, or hybrid transport.
Topology discovery tells participants which endpoints exist; transport moves
the data.

## Domain and topology

`CYBER_DOMAIN_ID` separates independent runtime domains. Topology discovery
announces nodes, channels, readers, writers, services, and clients inside a
domain.
