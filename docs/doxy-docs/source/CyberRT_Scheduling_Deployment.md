# Cyber RT Scheduling and Deployment Guide

## Scope

This guide covers deployment of Cyber RT's coroutine scheduler on machines with
different CPU topologies. It applies to both `classic` and `choreography`
policies, Linux IRQ placement, Iceoryx shared-memory pools, and runtime
observability.

The configuration must be selected from the actual machine topology. A static
configuration that assumes CPU IDs `0-31` is not portable: a process may be
restricted by a container or cgroup even when the host has those CPUs.

## Scheduler model

Cyber RT schedules stackful, cooperative coroutines. A coroutine releases its
Processor only when it yields, waits for data, sleeps, or enters the supported
IO wait path. A long computation, blocking syscall, synchronous RPC, GPU
synchronization, or third-party library call blocks every coroutine assigned to
that Processor.

`classic` creates shared priority queues for scheduling groups. `choreography`
adds dedicated Processor contexts for tasks with a configured `processor`;
tasks without a Processor enter the shared pool. In both policies, larger
`prio` values run first. Valid Classic priorities are `0-19`; values at or
above `20` are clamped to `19`.

`affinity: "range"` allows every Processor in a group to run on every CPU in
the listed set. It is not a one-thread-per-core policy. Use
`affinity: "1to1"` when a Processor must remain on the CPU at the same index in
its `cpuset`.

The scheduler supports these configurable internal thread names:

| Thread name | Purpose |
|---|---|
| `io_poller` | epoll thread used by `cyber::io::Session` |
| `timer` | timing-wheel thread |
| `shm_disp` | legacy shared-memory dispatcher |
| `async_log` | asynchronous logging thread |

Use `shm_disp`, not the obsolete example name `shm`, when configuring the
shared-memory dispatcher.

## Inventory the target machine

Run the following on every deployment target before choosing a profile:

```bash
lscpu -e=CPU,CORE,SOCKET,NODE,ONLINE
taskset -pc $$
cat /sys/devices/system/cpu/online
cat /sys/devices/system/node/node*/cpulist
```

The count of logical CPUs is not the count of physical cores. On SMT systems,
do not place two latency-critical Processor contexts on sibling logical CPUs.
Use a physical core's first sibling for critical work and reserve its sibling
for housekeeping, or disable SMT where the platform permits it. Keep NIC, GPU,
NVMe, and their consuming CPUs on the same NUMA node whenever possible.

Current scheduler validation rejects malformed CPU sets and logs failures from
`pthread_setaffinity_np`, `pthread_setschedparam`, and `setpriority`. A logged
affinity failure means the profile must not be accepted: Linux may otherwise
run the thread on an unrestricted CPU set.

## CPU profiles

Reserve resources before sizing Cyber Processor counts. The following examples
assume physical CPU IDs are contiguous and are examples only.

### Eight physical cores

| CPUs | Role | Scheduler assignment |
|---|---|---|
| `0` | IRQ and kernel housekeeping | No critical Cyber Processor |
| `1` | IO and transport | `io_poller`, `timer`, `shm_disp` |
| `2-3` | deterministic compute | Two choreography Processors, `1to1` |
| `4-7` | throughput compute | Four pool Processors, `1to1` |

Start with two choreography Processors and four pool Processors. Account for
RTPS, logging, middleware, driver, and system threads; eight CPUs must not be
treated as eight exclusively available user workers.

### Twelve physical cores

| CPUs | Role | Scheduler assignment |
|---|---|---|
| `0` | IRQ and kernel housekeeping | No critical Cyber Processor |
| `1` | IO and transport | Internal IO/transport threads |
| `2-5` | deterministic compute | Four choreography Processors, `1to1` |
| `6-11` | throughput compute | Six pool Processors, `1to1` |

GPU-heavy systems may need fewer CPU workers than this table suggests, because
CPU capacity is also required for DMA completion, GPU driver work, message
serialization, and post-processing.

Use a systemd `AllowedCPUs`/`CPUAffinity` policy or a cpuset cgroup to enforce
the process-level partition. Cyber's `process_level_cpuset` and per-thread
affinity complement, but do not replace, OS-level isolation.

## Classify tasks by execution behavior

Do not assign CPUs solely by an autonomous-driving module name. A single module
can contain socket waits, bounded transforms, GPU work, and recording. Classify
each coroutine by behavior, then map the class to a scheduling group.

| Class | Examples | Placement | Rules |
|---|---|---|---|
| Critical bounded compute | control state machine, time alignment | Dedicated choreography Processor | Bounded WCET; no blocking IO or long locks |
| Low-latency event work | decoding, small transforms, fusion ingress | Dedicated or small choreography set | Yield promptly; measure queueing delay |
| Throughput compute | inference, segmentation, compression, map work | Shared pool | Bound concurrency and queue depth |
| IO wait | socket RPC, disk, device wait | `cyber::io::Session` or dedicated IO thread | Use nonblocking APIs |
| Background | logging, recording, diagnostics | Low-priority pool or separate process | Rate-limit and make degradable |

Use `SCHED_OTHER` by default. `SCHED_FIFO` or `SCHED_RR` is only appropriate
after proving a task has a bounded runtime, cannot block, and has a watchdog or
safe degradation path. A runaway FIFO task can starve IO and system services.

## IRQ isolation

Place high-rate device interrupts on housekeeping or IO CPUs, never on the
critical choreography CPUs. On a low-rate platform one IRQ CPU may be enough;
for a high-rate multi-queue NIC use a small IRQ CPU set rather than forcing all
MSI-X vectors to one CPU.

```bash
grep -E 'eth|enp|can|nvme|usb' /proc/interrupts
echo 0 | sudo tee /proc/irq/<irq>/smp_affinity_list
cat /proc/irq/<irq>/smp_affinity_list

ethtool -l <interface>
ethtool -S <interface>
cat /sys/class/net/<interface>/queues/rx-*/rps_cpus
cat /sys/class/net/<interface>/queues/tx-*/xps_cpus
```

`irqbalance` can overwrite manual mappings. Disable it or configure its banned
CPU/IRQ policy, and persist mappings with a boot-time systemd unit or udev
rule. Verify both hard IRQs and `NET_RX` softirqs after each driver reload.

Kernel options such as `isolcpus`, `nohz_full`, and `rcu_nocbs` are advanced
system-level tuning. Introduce them only with a full validation of watchdogs,
drivers, RCU, clock ticks, and shutdown behavior.

## Shared-memory sizing

The default `cyber.pb.conf` selects `ICEORYX` for cross-process communication.
For this path, size the Iceoryx RouDi memory pools, not only legacy Cyber
POSIX/XSI shared-memory segments. The embedded RouDi adds a pool controlled by:

```bash
CYBER_ICEORYX_MEMPOOL_CHUNK_SIZE  # default: 8 MiB
CYBER_ICEORYX_MEMPOOL_CHUNK_COUNT # default: 16
```

The default additional pool is therefore 128 MiB per configured Iceoryx shared
memory segment, before Iceoryx defaults and metadata. It is suitable only when
the workload actually needs 8 MiB chunks. Prefer multiple size classes: many
small chunks for control data, medium chunks for structured data, and a bounded
large pool for images and point clouds.

For each size class:

```text
required chunks >= sum(rate * maximum_hold_time + maximum_burst) + safety_margin
pool bytes      = chunk bytes * required chunks
```

`maximum_hold_time` is the longest interval before a reader releases a chunk.
One sample can be shared by multiple readers, but a slow reader extends the
chunk lifetime. Leave RAM for application heaps, page cache, GPU pinned memory,
DDS, and failure recovery; do not allocate all available RAM to shared memory.

Measure real traffic before fixing pool sizes:

```bash
export CYBER_TRANSPORT_PROFILE_PATH=/var/log/cyber/transport-profile.toml
```

At shutdown Cyber writes maximum payload sizes and `write_busy_count` per
channel. A nonzero busy count indicates pool exhaustion, a slow consumer, an
oversized burst, or an overloaded downstream path.

## Observability

Enable only the layers required for the current diagnostic session:

| Layer | Tool or setting | What to watch |
|---|---|---|
| Cyber topology | `cyber_monitor`, then `f` for frame rate | missing writers/readers and stalled channels |
| Scheduler snapshot | `sysmo_start=1` | currently running coroutine and execution duration |
| Cyber event trace | `perf_conf { enable: true type: SCHED }` | scheduling and transport event timing |
| Transport capacity | `CYBER_TRANSPORT_PROFILE_PATH` | maximum payload and busy counts |
| CPU and scheduling | `pidstat -t -u -w -p <pid> 1`, `mpstat -P ALL 1` | CPU saturation, migrations, involuntary switches |
| Thread policy | `ps -Leo pid,tid,psr,cls,rtprio,pri,pcpu,comm`, `taskset -pc <tid>`, `chrt -p <tid>` | effective affinity and policy |
| IRQ and network | `/proc/interrupts`, `/proc/softirqs`, `ethtool -S` | IRQ placement, NET_RX load, packet drops |
| Shared memory | `df -h /dev/shm`, `ipcs -m`, `/proc/meminfo` | available capacity and leaks |

Track P99/P99.9 publish-to-callback latency, coroutine execution time, message
age, queue depth, drop count, transport busy count, CPU migration count, and
IRQ CPU utilization. Average CPU utilization is not sufficient evidence of
real-time behavior.

## Implementation plan

1. **Baseline and inventory.** Capture CPU/NUMA/cgroup topology, IRQ vectors,
   message rates, payload distributions, and end-to-end P99 latency on each
   target machine.
2. **Correctness safeguards.** Keep scheduler CPU-set validation enabled;
   reject deployment profiles that log affinity or policy-setting failures.
   Add a startup preflight that compares configured CPU IDs with the process's
   allowed affinity mask.
3. **Profile generation.** Define versioned 8-core, 12-core, and larger
   topology profiles. Generate CPU lists from physical-core and NUMA inventory,
   rather than assuming CPU numbering.
4. **Task classification.** Inventory every coroutine's WCET, frequency,
   blocking behavior, dependencies, and criticality. Map it to an execution
   class before assigning a group or dedicated Processor.
5. **IRQ and transport partitioning.** Persist IRQ/RPS/XPS mappings, pin
   internal IO threads, and verify that critical CPUs receive neither device
   IRQs nor unexpected kernel work.
6. **Capacity tuning.** Run representative peak and fault workloads with
   transport profiling. Size Iceoryx pools from measured in-flight chunks and
   slow-consumer behavior.
7. **Regression gates.** Gate profile changes on P99/P99.9 latency, no
   critical-path drops, no shared-memory busy events, and bounded critical CPU
   utilization. Test CPU hotplug, cgroup restriction, process restart, and
   slow-reader scenarios.
