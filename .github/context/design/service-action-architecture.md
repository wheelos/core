# Service and Action architecture

## Scope

Cyber should expose three communication primitives with intentionally different
semantics:

- **Topic** carries continuous data and control streams such as sensor data,
  localization, trajectories, chassis state, and actuator commands.
- **Service** performs short, bounded unary request/response operations such as
  state queries, mode transitions, configuration updates, and fault handling.
- **Action** manages long-running goals that need feedback and cancellation,
  such as parking, calibration, map loading, and software deployment.

Perception, planning, and control pipelines must not be chained with synchronous
services. Hard real-time, same-process calls should remain direct component or
function calls.

## Target architecture

Keep Fast DDS/RTPS as the transport and add a small RPC runtime above it:

```text
Application
  -> Service / Action API
  -> interceptors (deadline, admission, tracing, authorization)
  -> RPC runtime (pending calls, instance selection, bounded executors)
  -> discovery (instances, type/version, lifecycle, health)
  -> DDS/RTPS
```

Service V2 has the following required properties:

1. Each call has a unique call ID, a relative deadline, explicit status, and
   request/response type and interface versions.
2. A service is exclusive and serial by default. Replication and concurrency
   are explicit options.
3. Requests are routed to a selected instance-specific endpoint. Multiple
   servers must never execute a request merely because they share a topic.
4. Queues and in-flight calls are bounded. Overload returns
   `RESOURCE_EXHAUSTED`.
5. Automatic retries are disabled by default. Only declared idempotent methods
   may use bounded backoff and server-side deduplication.
6. Request and response DDS durability is `VOLATILE`; expired commands must
   never be replayed to late readers.
7. Readiness requires an active, healthy, type-compatible service instance.
8. Client and server registration is RAII-managed and removed during shutdown.

Action is built after Service V2 is stable. It combines short service calls for
goal acceptance and cancellation with topics for feedback and terminal result
delivery. Every goal has a unique ID, deadline, lifecycle, and explicit
preemption policy.

## Failure and safety model

The RPC layer reports typed failures such as `UNAVAILABLE`,
`DEADLINE_EXCEEDED`, `CANCELLED`, `RESOURCE_EXHAUSTED`,
`FAILED_PRECONDITION`, and `INTERNAL`. It does not silently retry vehicle
commands or choose a safety fallback.

The caller's state machine owns the vehicle response to failure. Mode changes,
fault resets, and other side-effecting operations do not retry automatically.
Long-running maneuvers use Action, and failure must transition the vehicle
supervisor to the operation-specific safe state.

## Implementation plan

### Phase 1: stabilize the existing Service API

- register pending calls before transmission and remove them on timeout or send
  failure;
- implement real readiness and correct finite/infinite wait behavior;
- make Client and Service destruction release transport resources and discovery
  roles;
- switch service QoS from transient-local to volatile;
- bound the server queue and reject overload;
- add focused lifecycle, timeout, readiness, and burst regression tests.

This phase preserves `CreateService`, `CreateClient`, `SendRequest`, and
`AsyncSendRequest`.

### Phase 2: introduce Service V2

- add `RpcStatus`, `RpcResult`, `RpcContext`, `RpcCallOptions`, and
  `ServiceOptions`;
- carry call ID, relative deadline, cancellation, type hash, interface version,
  trace context, and idempotency metadata;
- replace single-value discovery with explicit service instances and lifecycle
  states;
- route requests to instance-specific endpoints;
- use shared bounded executors rather than one unbounded thread per service;
- add interceptor hooks and RPC metrics/traces.

The existing API becomes a compatibility facade over V2.

### Phase 3: add Action

- define goal, feedback, result, cancellation, and preemption semantics;
- implement `ActionClient` and `ActionServer` on Service V2 plus topics;
- cover cancellation races, server loss, goal replacement, deadline expiry, and
  bounded feedback flow;
- migrate parking, calibration, map loading, and similar long operations.

### Phase 4: production hardening

- add authorization, rate limits, audit metadata, and security identities;
- support bounded idempotent retry and deduplication;
- validate process restart, network partition, overload, and degraded-service
  matrices;
- add SOME/IP or edge/cloud gateways behind the same application API where
  interoperability requires them.

## Release gates

- existing service burst and multi-client integration coverage;
- no pending-call growth after timeouts;
- no historical request replay when a server joins;
- deterministic duplicate-server behavior;
- bounded queue and overload behavior;
- cancellation and deadline propagation tests before Action adoption.

## Current implementation status

Phase 1 is implemented. Pending calls are registered before transmission and
removed on timeout, readiness reflects discovery, discovery roles leave through
RAII cleanup, service queues are bounded, callbacks run outside client locks,
and service durability is volatile.

The first compatible Service V2 slice is also implemented:

- `RpcStatus`, `RpcResult`, `RpcCallOptions`, `RpcContext`, and
  `ServiceOptions` are available;
- services publish request/response types, interface versions, instance IDs,
  and instance-specific request endpoints through discovery;
- clients reject incompatible types or major versions and route requests to
  the discovered instance endpoint;
- exclusive duplicate services are rejected;
- queue capacity and handler concurrency are configurable;
- the typed `Call` API reports local validation, availability, and deadline
  errors while the legacy API remains available.

The V2 wire envelope is now implemented. Request payloads carry call ID,
relative timeout, priority, trace ID, and idempotency key; responses carry
typed status, error detail, and payload. The server creates an `RpcContext`
using the received budget, rejects work that expires in the queue, and returns
handler status to the typed client API. Legacy callbacks are adapted to an OK
status and retain their source API.

Service V2 now also supports explicit in-flight cancellation, cooperative
server cancellation through `RpcContext`, bounded idempotency result caching,
multi-instance discovery, deterministic client-side instance sharding, and
failover when an instance leaves. Exclusive services remain the default;
replication must be selected explicitly by every instance.

The core Service V2 protocol is considered ready for Action development.
Interceptor-based observability and authorization remain production-hardening
work and must not change the established wire contract.

## Current validation baseline

The focused Service and Action regression set passes: Action context, RPC
status/context, service discovery, service lifecycle/callback/V2/replication,
parameter client, and example integration/stress targets. The canonical
release baseline remains the required final gate before release.

Action implementation has started. The checked-in wire contract now defines
goal acceptance, cancellation, feedback, terminal result, and goal lifecycle
states. `ActionOptions` provides bounded-goal and preemption defaults, while
`ActionContext` exposes deadline-aware cancellation and feedback publication.

## Action implementation backlog

The Action wire contract, `ActionOptions`, and `ActionContext` are implemented
and tested; the runtime is intentionally not yet exposed through `Node`.

1. Implement direct `ActionServer`/`ActionClient`: goal and cancel use Service
   V2; feedback and terminal results use action-scoped Topics.
2. Enforce bounded goals and all preemption modes; cancellation must be
   cooperative and yield exactly one terminal result.
3. Add Node facades and discovery metadata after direct-template tests cover
   every terminal state, cancellation race, server loss, feedback ordering,
   and burst limits.

Action must remain the sole API for long-running tasks such as parking,
calibration, map loading, and software deployment. Do not model those flows
as long-running Services.
