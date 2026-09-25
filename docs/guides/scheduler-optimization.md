# Discovering and Tuning Cyber Scheduler Policies

This guide describes a measurable way to choose and tune Cyber scheduler
policies. There is no scheduler configuration that is optimal for every
workload. The goal is to find a configuration that meets explicit service
objectives without wasting CPU capacity or starving less critical work.

## Define the objectives

Scheduler tuning involves trade-offs:

- Lower latency may require more CPU capacity or stronger isolation.
- Higher throughput can increase latency variation.
- Fewer processors reduce resource use but can lower peak capacity.
- Aggressive real-time policies can starve ordinary tasks.

Define service-level objectives (SLOs) before tuning. Include the metrics that
matter for the workload:

| Metric | What it measures |
| --- | --- |
| Period error | Deviation from the target task period |
| End-to-end latency | Time from input arrival to output |
| P95/P99 latency | Tail latency, often more informative than the mean |
| Maximum latency | Worst observed jitter or deadline miss |
| Execution time | CPU time consumed by the task |
| Queue wait time | Time a task waits before it is scheduled |
| CPU utilization | Total, per-processor, and per-CPU utilization |
| Drop rate | Lost frames or messages under load |

For example, a control path might require zero deadline misses and a P99 below
10 ms, while an offline workload might prioritize total throughput. Without
defined objectives, a tuning change cannot be judged as an improvement.

## Choose a scheduler model

Cyber provides Classic and Choreography scheduler policies. Their
configuration is defined in
[`classic_conf.proto`](../../cyber/proto/classic_conf.proto),
[`choreography_conf.proto`](../../cyber/proto/choreography_conf.proto), and
[`scheduler_conf.proto`](../../cyber/proto/scheduler_conf.proto).

### Classic: processor groups and shared task queues

Classic assigns a set of processor threads to each group. Tasks in a group
share its priority queues. It is a good starting point for:

- Many tasks with variable load.
- Workloads without stable profiling data.
- General compute tasks that can share a thread pool.
- Systems where simple, maintainable configuration is preferred.

Establish a Classic baseline before considering fixed task placement.

### Choreography: assigned task processors plus a pool

Choreography can assign named tasks to processors while leaving other tasks in
a processor pool. Consider it for:

- Critical paths with explicit deadlines.
- Tasks with stable, measurable execution times.
- Workloads sensitive to P99 latency or scheduling jitter.
- Tasks shown by profiling to be affected by processor-pool contention.

Do not pin every task. Too many fixed assignments can overload some processors
while leaving others idle and increase coupling between task names and
configuration.

> Pin a small number of stable, critical tasks; leave bursty or noncritical
> work pooled.

## Profile the workload

Before changing scheduler settings, record each CRoutine's:

```text
task name
input frequency and target period
mean, P95/P99, and maximum execution time
whether load is bursty
whether frames may be dropped
deadline and critical-path membership
whether it blocks on I/O
```

For a periodic task, estimate its CPU demand as:

```text
U_i = C_i / T_i
```

Here, `C_i` is execution time and `T_i` is the task period. A task that uses
2 ms of CPU every 10 ms needs about 20% of one CPU. Keep the total demand on a
processor below its capacity and leave headroom for execution-time variation,
message handling, system threads, and bursts. Use P99 or a conservative
worst-case execution time for critical-path capacity planning; averages alone
hide tail behavior.

## Establish a Classic baseline

Start with:

- `policy: "classic"`.
- `SCHED_OTHER` for processor threads.
- A small number of purpose-based groups such as `critical`, `compute`, and
  `background`.
- Separate critical work from long-running or bursty background work.
- A processor count sized for the available CPU capacity and workload; do not
  assume more processors always improve performance.
- Priorities based on deadlines and dependencies, not a blanket maximum.

For example:

```text
critical:   localization, planning, control
compute:    perception, prediction, fusion
background: compression, recording, visualization
```

Use this baseline to identify overloaded groups, task interference, long queue
waits, saturated or idle processors, and long tasks blocking critical work.

## Tune one dimension at a time

Apply changes incrementally so that each result can be attributed to a
specific change. A useful order is:

```text
task grouping
  -> CPU affinity
  -> processor count
  -> Cyber task priority
  -> fixed placement for a few tasks
  -> Linux scheduling policy
```

### CPU affinity

`range` lets a processor run on the configured CPU set. `1to1` maps each
processor to the corresponding CPU in that set. With `1to1`, the processor
count must not exceed the number of CPUs in the set.

An initial layout might reserve separate CPU sets for critical work, compute
work, background pools, and the operating system. Check physical cores,
SMT/hyperthreading, heterogeneous cores, NUMA topology, container CPU quotas,
cpusets, and cgroup limits before assigning CPUs. Treat example ranges as
illustrations, not recommendations for every machine.

### Cyber task priorities

Classic task priorities range from 0 to 19; higher values are selected first.
Use a small number of priority tiers based on deadlines and dependencies. For
example:

```text
18-19: urgent control and safety tasks
15-17: localization, planning, and critical fusion
10-14: primary perception tasks
5-9:   ordinary application work
0-4:   logging, visualization, and auxiliary work
```

These are starting points, not universal settings. Priority does not create
CPU capacity: on a saturated processor, raising one task's priority only
changes which tasks wait.

## Isolate only demonstrated bottlenecks

Use Choreography only after reasonable Classic grouping and CPU isolation are
in place and measurements still show problematic tail latency on a critical
task. A task is a candidate for fixed placement when most of these are true:

- It is stable, critical, and has an explicit deadline.
- Its execution-time variation and CPU demand are understood.
- It does not spend long periods blocked on I/O.
- Profiling shows that processor-pool contention is a significant cause.

Tasks assigned to the same Choreography processor still run serially. Avoid
placing multiple heavy tasks on one processor without measuring their combined
demand. Tasks not assigned to a dedicated processor use the pool; size it for
that work.

## Use real-time policies as a last step

Start with ordinary scheduling:

```protobuf
processor_policy: "SCHED_OTHER"
processor_prio: 0
```

Consider `SCHED_FIFO` or `SCHED_RR` only after verifying that scheduling jitter
is the bottleneck, and apply them to as few processors as possible. Before
deployment, validate:

- Watchdogs and execution-time limits.
- Protection against unbounded tasks.
- Starvation of lower-priority tasks.
- Logging, communication, and shutdown behavior.
- Required privileges and startup failure handling.

`SCHED_FIFO` is not a general performance switch. A high-priority thread that
does not block can monopolize a CPU and prevent communication, logging, or
ordinary tasks from running.

## Design repeatable experiments

Each experiment should change one variable. Test processor count, group
structure, affinity/cpusets, task priority, Choreography assignments, and
Linux scheduling policy in separate steps.

Cover representative conditions:

- Idle, normal, and peak load.
- Bursty sensor input.
- Recording and replay.
- Network or I/O jitter.
- Longer-than-usual task execution.
- CPU pressure from other processes or container limits.

For large configuration spaces, generate candidates and rank them with a
workload-specific score, for example:

```text
Score =
    latency penalty
  + deadline-miss penalty
  + CPU-overload penalty
  + drop penalty
  + processor-count cost
```

Give deadline misses a dominant penalty for control workloads. For throughput
workloads, give throughput more weight. A coarse grid or random search can
identify a feasible region before manual refinement.

## Observe and verify

Cyber's `CheckSchedStatus()` can report active routines and execution times.
Useful system-level tools include:

```bash
ps -L -o pid,tid,psr,cls,rtprio,pri,ni,stat,comm -p <pid>
taskset -cp <tid>
chrt -p <tid>
pidstat -t -p <pid> 1
perf sched timehist
perf stat -p <pid>
```

Verify that threads have the intended CPU affinity and Linux policy, check for
excessive migration and context switching, and look for saturated processors
or tasks that rarely run.

Task names in the scheduler configuration must match the runtime CRoutine
names. Confirm names from the scheduler's `create croutine: ...` log rather
than guessing from DAG labels. Both scheduler policies load
`conf/<ProcessGroup>.conf`; for process group `compute_sched`, check
`conf/compute_sched.conf`. Verify the selected scheduler policy and runtime
logs rather than assuming that the configuration took effect.

## Roll out and regress

1. Record a Classic/`SCHED_OTHER` baseline with P95/P99 latency, maximum
   latency, execution time, and CPU utilization.
2. Separate critical, compute, and background work; tune processor counts and
   priorities.
3. Apply CPU affinity to isolate the demonstrated bottlenecks.
4. Pin only a few stable critical tasks with Choreography and verify that no
   processor is overloaded.
5. Consider real-time policies only with watchdogs and failure handling.
6. Re-test peak, burst, and fault conditions, then keep the configuration and
   its measured objectives in regression.

Scheduler configuration must be revisited when task execution times, input
rates, CPU models, container limits, or workload assumptions change.

## Related implementation and examples

- [`SchedulerClassic`](../../cyber/scheduler/policy/scheduler_classic.cc)
- [`SchedulerChoreography`](../../cyber/scheduler/policy/scheduler_choreography.cc)
- [`example_sched_classic.conf`](../../cyber/conf/example_sched_classic.conf)
- [`example_sched_choreography.conf`](../../cyber/conf/example_sched_choreography.conf)
- [Performance testing](performance-testing.md)
