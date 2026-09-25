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
- The optional `cyber/transport/nvsci` GPU channel supports one writer per
  channel per host and multiple readers. `GpuChannelManager` holds a process
  lease backed by `flock` in a validated user-private directory under
  `XDG_RUNTIME_DIR` (falling back to `HOME`); processes that must coordinate
  across containers need that directory on the same shared filesystem. This
  lock is setup/lifecycle coordination, not part of the frame data path.
- GPU slot ownership is fence-based: producer writes must be ordered on the
  loan's CUDA stream, unpublished loans fence that stream before returning the
  slot, and readers acknowledge only after their consumer-stream work is
  queued. Timeout or consumer loss quarantines outstanding slots rather than
  proving GPU work has completed.
- Fast-DDS participant teardown requires strict phased unpairing: for each
  domain participant, all Subscribers (readers) must be removed first before
  Publishers (writers) are removed, followed by removing the Participant itself.
  This avoids use-after-free (UAF) during reader proxy unpairing.
- Global teardown sequence in `FinishClear`: Transport cleanup (subscribers ->
  publishers -> transport participant) precedes TopologyManager cleanup
  (discovery subscribers -> discovery publishers -> topology participant).
- The scheduler turns component reader work into tasks; component authors
  implement `Init` and `Proc` rather than taking over framework `Process`.

## Sources

- `cyber/cyber.h`
- `cyber/init.cc`
- `cyber/node/`
- `cyber/mainboard/`
- `cyber/transport/`
- `cyber/transport/nvsci/gpu_channel_manager.*`
- `cyber/transport/nvsci/gpu_channel_session.*`
- `cyber/transport/nvsci/gpu_writer.h`
- `cyber/service_discovery/`
- `cyber/scheduler/`
