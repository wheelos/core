# WheelOS Core 1.1: current runtime behavior

This is a characterization of the existing C++ channel runtime, not a new
delivery guarantee or a promise that all transports behave alike. Each row
distinguishes implementation observations from test-backed behavior;
`unknown` means the API cannot establish the claim. Changes to these facts
require updating adjacent tests before changing the documented contract.

## Writer: submission is not delivery

| Surface | Current behavior | Evidence and boundary |
| --- | --- | --- |
| `Write(const M&)` | Allocates a `shared_ptr<M>` containing a copy before calling the transmitter. | `cyber/node/writer.h` (`Write(const M&)`); `//cyber/node:writer_test` (`ValueCopiesSharedPointerPreservesIdentityAndResult`). Further backend copies/serialization depend on the selected transport. |
| `Write(shared_ptr<M>)` | Passes the shared message object to the transmitter without copying the object at this layer. | Same source/test. This does **not** promise zero-copy on SHM, RTPS, or another backend. |
| `Write()` result | Returns the synchronous transport submission result; returns `false` if the Writer is no longer initialized. It does not wait for delivery, enqueue, or callback completion. | `cyber/node/writer.h`; `//cyber/node:writer_test`. There is no bounded execution-time guarantee: the call may allocate, serialize, lock, or block in a backend. |
| HYBRID with no active reader | Returns `true`, even though no backend receives the message. With default volatile durability, no history is retained for later replay. | `cyber/transport/transmitter/hybrid_transmitter.h` (`Transmit`, `InitHistory`), `cyber/transport/message/history.h` (`Add`); `//cyber/transport/integration_test:hybrid_transceiver_test` (`WriteResultIsAnyActiveBackendAccepted`). History replay is conditional on configured transient-local durability; `true` is never a delivery receipt. |
| HYBRID with multiple active backends | Attempts each active backend. Returns `true` if **any** accepts; returns `false` if all reject. A partial rejection is logged but not encoded in the bool result. | Same HYBRID source/test. Which reader received or processed the message is **unknown**. |
| `Loan` / `Publish` | Delegate to the selected transmitter. HYBRID only loans/publishes when active receivers use a single supported backend (ICEORYX or SHM). | `cyber/node/writer.h`, `cyber/transport/transmitter/hybrid_transmitter.h`; `//cyber/node:writer_test` (shutdown races), `//cyber/transport:transport_test` (POD loans). No general loan or zero-copy promise for arbitrary message types/routes. |

Do not reinterpret `Write() == true` as `no_route == false`, reliable delivery,
or successful callback. A future detailed result must obtain each backend
outcome from the backend itself, without transmitting a second time.

## Reader: local queue and callback

| Surface | Current behavior | Evidence and boundary |
| --- | --- | --- |
| Payload and time | The callback receives a `shared_ptr<M>`; application message fields (including any timestamp) remain application-defined. `Reader::Enqueue` records local `Time::Now()` for `GetDelayNs()`, which is **not** a source timestamp or transport latency. | `cyber/node/reader.h` (`Enqueue`, `GetDelayNs`); `//cyber/node:writer_reader_test` (`get_delay_sec`). A source clock domain and cross-host time correlation are **unavailable**. |
| Pending queue | Default `pending_queue_size` is 1. The first `ChannelBuffer::Fetch` selects the newest item; if an unread cursor falls behind the ring head, it skips to the newest surviving item. | `cyber/node/reader.h`, `cyber/data/channel_buffer.h`; `//cyber/node:writer_reader_test` (`constructor`), `//cyber/data:channel_buffer_test` (`FirstFetchAndOverflowBothSkipToLatest`). A successful write does not ensure this reader will invoke its callback. |
| Ordering | Consumption follows the local buffer cursor for items that remain readable. Initial fetch and overflow can skip older items; no total order across writers, backends, reconnects, or processes is guaranteed. | `cyber/data/channel_buffer.h`; `//cyber/data:channel_buffer_test`; `//tests/integration_test:cross_process_churn_test` checks per-writer progress, **not** cross-writer order. |
| Callback lifetime | `Reader::Shutdown()` removes its scheduler task and waits for a callback already executing to return; pending messages need not run their callbacks. No callback deadline or forced cancellation exists. | `cyber/node/reader.h` (`Shutdown`), `cyber/scheduler/policy/classic_context.cc` (`RemoveCRoutine`); `//tests/integration_test:runtime_metrics_test` (`ReaderShutdownCountsUnreadQueueAfterInflightCallback`). This is a graceful-shutdown observation, not a guarantee after SIGKILL. |

Reader's pending callback queue and the `Observe()` blocker history are
different resources. `SetHistoryDepth()` changes blocker retention, not
`pending_queue_size` or the scheduler's callback queue.

## Lifecycle and recovery boundaries

| Resource | Current ownership/ordering | What remains unverified |
| --- | --- | --- |
| Writer | `Shutdown()` unpublishes topology and clears its own transmitter reference. A write/loan/publish that already captured a `shared_ptr` can finish afterward; later writes return `false`. | `//cyber/node:writer_test` checks in-flight transmitter lifetime. `Writer::Shutdown()` is **not** a completion barrier for already-started submissions. |
| Reader/task | Reader leaves topology, releases receiver ownership, then removes the scheduler task; graceful task removal waits for an executing callback. | No fixed shutdown deadline or safe forced callback cancellation. |
| Framework | `Clear()` stops scheduler work before `Transport`, then `TopologyManager`; transport shuts down dispatchers before its RTPS participant and embedded RouDi. Python exit has a separate cleanup path. | `cyber/init.cc` (`FinishClear`, `ClearForPythonExit`), `cyber/transport/transport.cc` (`Shutdown`). This ordering is an implementation observation, not yet covered by a targeted teardown-order assertion. A process killed without cleanup cannot promise graceful release. |
| Crash/restart | Existing churn tests verify bounded restart and delivery in their exercised processes. | SIGKILL/OOM, host reboot, stale resource reclamation, GPU fence completion, and old-epoch ACK rejection need separate fault-injection evidence; do not infer safe recovery from graceful tests. |

The current release/metrics validation gates and their outstanding results
are tracked in
[`runtime-contract-rollout-plan.md`](../../.github/context/design/runtime-contract-rollout-plan.md).
This document does not claim old-plugin ABI or N-1 wire compatibility.
