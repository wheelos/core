# WheelOS Core Runtime Contract 演进路线图

状态：规划基线。本文基于 2026-09-28 当前实现，优先定义可验证的
Semantic、Lifecycle、Compatibility 和 Recovery Contract，再演进性能与功能。
Runtime Metrics 的具体指标计划仍见
`../design/runtime-metrics-plan.md`。近期兼容优先的实施拆分与验收门槛见
`../design/runtime-contract-rollout-plan.md`；当前可验证的 Writer/Reader
语义见 `../../../../docs/guides/runtime-contract-v1.md`。

## 2026 Q4：Core 1.1 基线，不扩功能面

WheelOS Core 的定位是面向自动驾驶与机器人端侧的 Runtime：
**High Performance + Deterministic + Observable + Recoverable + Portable**。
这些是长期演进方向，不是对当前所有后端或通用 Linux 环境的保证。
从第一性原理看，Runtime 的价值不是后端或选项数量，而是调用者能否
依赖明确的行为、故障时能否解释原因、变更后能否证明没有退化。
Q4 因此只投入三条相互制约的主线：

| 主线 | Q4 要解决的问题 | Core 1.1 证据 |
| --- | --- | --- |
| Correctness | 已知竞态、退出、重启、队列覆盖与后端部分失败能否稳定复现和正确处理 | 单元/跨进程/故障注入的行为矩阵；非缓存容器冷构建与完整运行矩阵；故障不得伪装成成功 |
| Contract | `Write`、Reader、资源所有权与发布身份究竟保证什么 | 逐项可追溯的语义表和测试；明确 unsupported/unknown；同源构建的 deb/wheel/sdist 版本与安装验证 |
| Observability | 异常能否定位且观测本身是否改变热路径 | 现有 opt-in Metrics 的正确性、标签/内存边界；关闭与开启模式在固定 runner 上的可复现开销和尾延迟证据 |

**范围冻结**：不在 1.1 引入新的 Queue/QoS/Scheduler/Replay
策略、传输后端、跨进程 metadata 或公开的结构化 Writer/Reader API；
先把现有行为表征、文档化、测量和修正。修复已复现的缺陷可以改
实现，但不得暗改默认语义；若不得不改变行为，单独说明迁移与回退。
下面 Phase 1 及以后描述的是 **1.1 之后的候选演进**，不是 Q4
承诺。旧插件 ABI 边界本轮不处理，也不声称旧二进制兼容。

执行顺序：先冻结源码与环境、关闭已知容器验证缺口；再冻结语义与
资源所有权的可测试事实；最后在固定 runner 校准预算并验证
off/basic/detailed。每个交付必须能回答：

1. 哪个已复现问题或明确的不变量需要它？没有证据的功能请求延后。
2. 谁拥有状态、资源和故障处理？不得增加第二套可编辑的版本、
   状态机或配置来源。
3. 默认关闭时是否无逐消息分配、计时、动态标签或额外锁？
   开启时内存、CPU 和失败路径是否有界且可测量？
4. 是否有最近包单测、跨进程反例、稳定环境对照及回退边界？
   维护成本无法说明或验收无法自动化的改动不进入 1.1。

Core 1.1 是**可验证的基线**，不是“已经全局确定性/可恢复”
的宣称。两小时运行、资源失效/重启和固定 runner 性能结果
须单独列出已覆盖的环境与场景；未完成项保持未验收。
本轮按要求跳过 Valgrind，泄漏门槛记为未验收，不用其他
测试替代泄漏结论。1.1 具体排序与停止条件见近期实施计划。

## 1. 结论与方向

WheelOS Core 的下一阶段不应以“增加更多传输能力”为主线，而应成为面向
自动驾驶与机器人端侧的：

> High Performance + Deterministic + Observable + Recoverable + Portable

优先级建议：

1. **Semantic Contract**：先明确 API 的成功、所有权、时间、顺序、背压和关闭语义。
2. **Lifecycle Contract**：把隐含在析构顺序和资源所有权中的约束变成状态机。
3. **Compatibility Contract**：统一版本身份，定义 wire/schema/API/ABI 的兼容矩阵。
4. **Recovery Contract**：明确 graceful shutdown、进程崩溃和主机重启各自能恢复什么。
5. **Low-overhead Observability**：观测必须服从明确的 CPU、内存和尾延迟预算。
6. **Determinism and tooling**：在契约稳定后提供预算、诊断、复现和运维工具。

“零拷贝”“更多指标”“更多后端”都是实现手段，不是用户可依赖的契约。
“可恢复”也不能模糊承诺：SIGKILL 无法执行进程内清理，C++ 回调不能被框架
安全强杀。必须区分正常关闭保证、故障检测保证和外部恢复保证。

## 2. 当前事实与契约缺口

### 2.1 Writer 当前实际语义

| 问题 | 当前实现 | 必须补齐的契约 |
| --- | --- | --- |
| `Write(const M&)` 是否复制 | 先 `make_shared<M>(msg)`，至少发生一次对象复制/分配；后端还可能序列化复制 | API 文档明确 ownership 和每个后端允许的复制类型 |
| `Write(shared_ptr<M>)` 是否零拷贝 | INTRA 共享指针；SHM/RTPS/普通 ICEORYX 仍序列化；只有受支持的 Loan/Publish 路径具备借用语义 | 不用统一的 “zero-copy” 标签掩盖不同路径 |
| 是否立即完成 | API 同步返回，但内部可执行分配、锁、序列化、SHM block acquire、通知和 DDS `write` | 明确“同步提交”不等于有界耗时或远端完成 |
| 是否可能阻塞 | 可以；当前没有公开的最大阻塞时间保证 | 增加可配置的 nonblocking/bounded-blocking 策略，而不是默认宣称实时 |
| `true` 表示什么 | HYBRID 无 active receiver 时返回 true；它会调用 history `Add`，但默认 volatile 不缓存。多后端时任一后端成功即返回 true，部分失败只记日志 | 将 `accepted`、`submitted`、`delivered`、`processed` 分层 |
| 是否可能丢 | 可以；队列、SHM 资源、QoS、进程退出和部分后端失败均可能导致未送达 | 每层分别定义 drop reason 和可观测性 |
| 顺序 | 单 transmitter 有本地 sequence；跨 Writer、跨后端或重连没有统一全序保证 | 定义 per-writer FIFO、跨 writer 无序及重连 epoch |

因此，现有 `bool Write()` 只能保守解释为：

> 当前 Writer 接受了调用，并且在存在 active backend 时至少一个 backend
> 接受了同步提交；它不证明任一 Reader 已接收或 callback 已完成。

为保持兼容，短期不改变 `bool Write()`，先补文档和契约测试；随后新增
`WriteResult`/`PublishResult`，显式返回：

```text
status: accepted | no_route | resource_exhausted | serialization_error |
        backend_error | shutting_down
accepted_backends
failed_backends
sequence
```

如果未来需要 delivery guarantee，应使用独立的异步 receipt/future：

```text
SUBMITTED -> TRANSPORT_ACKED -> READER_ENQUEUED -> CALLBACK_COMPLETED
```

不同 QoS 和后端只实现自己能够证明的最高层级，不能用 API 成功冒充送达。

### 2.2 Reader 当前实际语义

| 问题 | 当前实现 | 必须补齐的契约 |
| --- | --- | --- |
| `msg.timestamp` | 普通消息字段由业务定义；框架没有把它统一定义为 source 或 receive time | framework metadata 独立携带 source/receive/steady time 及 clock domain |
| 顺序 | 队列按到达/dispatch 顺序消费，但初次 Fetch 取最新，溢出后也跳到最新 | 明确 per-writer ordering、重连 epoch、乱序检测和 policy skip |
| 队列满 | 默认深度 1；环形缓存覆写，消费者落后时跳到最新 | 把 `keep_latest`、`keep_all`、reject/block 等策略显式配置化 |
| callback 超时 | 没有强制超时或抢占；Metrics 只能观测耗时 | 提供 budget/overrun 事件和 cooperative cancellation；不得强杀 C++ callback |
| shutdown | RemoveTask 会等待正在运行的协程释放；当前集成测试验证 callback/Proc 返回后 shutdown 才完成 | 文档化同步 quiescence；增加 deadline/timeout 只能返回状态，不能假装已安全终止 |
| payload 生命周期 | callback 接收 `shared_ptr`，业务可在 callback 后继续持有 | 明确 payload、loan 和 GPU handle 的持有期限分别不同 |

建议引入只读 `MessageContext`，但保持旧 callback 可继续使用：

```text
writer_id / writer_epoch / sequence
source_time + source_clock_domain
receive_steady_time
transport_path
ordering_status
```

该 metadata 必须版本化、可选并可在旧节点间降级；不能强迫每条业务消息修改
protobuf schema，也不能在无法证明时生成伪 source time。

### 2.3 Lifecycle 当前实际复杂度

当前实现已经存在多套隐式状态机：

- Fast DDS 数据 participant 必须按 subscriber -> publisher -> participant 退订；
- `FinishClear` 要先清 Transport，再清 TopologyManager；
- Reader/Component shutdown 要等待正在运行的协程退出；
- GPU Writer/Reader 有 process lease、session id、slot ownership、fence、
  ACK、consumer lease、timeout 和 quarantine；
- SHM/ICEORYX 还涉及进程退出后的共享资源与 daemon 生命周期。

这些约束不应继续依赖析构函数、调用方记忆和分散注释。目标模型：

```text
CREATED -> STARTING -> RUNNING -> QUIESCING -> DRAINING
        -> STOPPED -> RELEASED
                    \-> FAILED -> RECOVERING
```

每个资源类型必须声明：

- owner、borrower 和跨进程 lease；
- 创建/发布/撤销/释放的合法状态转换；
- idempotent 操作；
- shutdown dependency；
- graceful deadline 到期后的结果；
- crash 后的 stale 判定、隔离和回收方式；
- generation/epoch，防止新进程误接管旧资源。

### 2.4 Compatibility 当前风险

版本身份曾出现真实漂移（已加入发布校验，正式发布前仍须验证发布产物）：

- `MODULE.bazel`：`1.0.5`；
- 根 `BUILD` 的 Debian 包：原为 `1.0.4`，现对齐为 `1.0.5`；
- pycyber：由 `wheelos_core-v<version>` tag / `setuptools_scm` 动态生成，
  无匹配 tag 时会产生 dev version；
- GitHub 已发布 `1.0.0` 至 `1.0.5`，其中 `1.0.5` 发布于
  2026-09-14。

这种漂移会导致 tag、Bazel module、Debian、Python wheel、运行时报告和
支持矩阵互相矛盾；当前实现已增加 release 版本一致性门禁。

版本不能只是一串 release number。至少分开：

```text
release_version
public_api_version
wire_protocol_version
metrics_schema_version
record_format_version
plugin_abi_version
```

## 3. 目标架构

```text
Application API
  ├── Semantic Contract
  │    ├── ownership / copy / loan
  │    ├── submit / delivery / processing result
  │    ├── ordering / time / backpressure
  │    └── callback / shutdown guarantee
  │
Runtime Lifecycle Coordinator (only if local teardown fixes fail)
  ├── dependency graph
  ├── quiesce + drain
  ├── lease / epoch / ownership
  └── fault recovery policy
  │
Transport + Scheduler + Storage
  ├── INTRA / SHM / RTPS / ICEORYX / GPU
  └── explicit backend capabilities
  │
Low-overhead Telemetry Plane
  ├── bounded counters / conditional per-thread shards
  ├── periodic aggregation
  ├── immutable snapshot
  └── versioned export schema
  │
Devtools
  ├── monitor
  ├── profiler
  └── diagnostics / recovery inspector
```

Devtools 可以先留在当前仓库，复用同一 schema 和发布门禁；只有协议稳定且需要
独立发布节奏时，再拆为 `wheelos/devtools`，避免过早产生兼容矩阵。

## 4. 分阶段计划

### Phase 0：契约基线与发布卫生（立即）

交付：

1. 建立 `contracts/` 文档和 machine-readable capability 表：
   - Writer/Reader v1；
   - transport backend capability；
   - lifecycle state/ownership；
   - compatibility matrix。
2. 将现有行为冻结为 characterization tests，不先“改善”行为：
   - 无 reader 时 `Write`；
   - HYBRID 部分成功；
   - const/ref/shared/loan 的复制与所有权；
   - 初次 Fetch、overflow、shutdown 中 callback；
   - 重连后的 sequence/ordering。
3. 建立唯一 release identity：
   - 以现有 `MODULE.bazel` 版本作为校验输入，不新增第二个
     可手工维护的版本文件；
   - MODULE、Debian、pycyber 和 artifact manifest 必须由
     它派生或由 CI 强校验；如果已有 runtime `--version`
     入口，也须校验其报告的版本；
   - tag 必须为 `wheelos_core-v${version}`；
   - 修正现有 1.0.5/1.0.4 漂移及相邻 release script 测试。

验收：

- 文档中的每项 MUST/SHOULD 都有测试或明确标为 unsupported；
- release 流程在版本不一致时构建失败；
- 不修改 wire format 和默认行为；不新增未经验证的 ABI 承诺，
  现有工作区的旧插件二进制兼容不在本轮验收范围。

### Phase 1：Semantic Contract v1

交付：

1. 为 Writer 增加非破坏性的 structured result API，保留 `bool Write()`。
2. 定义 backend capability：
   - copy model；
   - loan support；
   - blocking model；
   - durability/history；
   - 可证明的 delivery level；
   - ordering scope。
3. Reader 增加可选 `MessageContext` callback overload。
4. 将 queue policy 从隐式环形行为提升为显式枚举，并保持默认
   `keep_latest(depth=1)` 兼容。
5. callback budget 只产生 overrun/diagnostic 和 cooperative stop token；
   不提供不安全的线程强杀。

验收矩阵：

- INTRA/SHM/RTPS/HYBRID/ICEORYX/GPU；
- 无订阅、单订阅、fanout、后端部分失败；
- normal/shared/loan；
- best-effort/reliable；
- overflow、重连、shutdown race；
- 新 API 与旧 API 对同一提交结果保持一致。

### Phase 2：Runtime Lifecycle Model

交付：

1. 先通过依赖图、顺序断言与故障注入验证现有
   quiesce/drain/release；仅在局部修正仍不能解决已复现问题时
   试点 Lifecycle Coordinator，替代对应的隐含销毁顺序。
2. 为 Node、Reader、Writer、Task、Transport、Topology、GPU session、
   exporter 定义状态机和幂等操作。
3. 将 Fast DDS subscriber -> publisher -> participant 及
   Transport -> Topology 顺序编码为可测试 dependency。
4. shutdown 返回结构化结果：

```text
clean | drained_with_discard | deadline_exceeded | quarantined | failed
```

5. 所有后台线程都必须声明 stop signal、join owner 和 deadline 行为。

验收：

- 重复 Init/Shutdown/Clear；
- create/delete/notify 并发；
- callback/Proc 阻塞；
- Fast DDS unpairing 与 participant teardown 压力；
- sanitizer/valgrind 无 UAF、double free、泄漏；
- lifecycle transition 可通过低频事件观测，不在消息热路径记录字符串。

### Phase 3：Recovery Contract

交付：

1. SHM segment 增加 owner identity、process start identity、epoch、
   schema/version 和最后活动时间。
2. 提供只读 inspector 和显式 reaper：
   - 能区分 active/stale/corrupt；
   - 默认不回收无法证明 stale 的资源；
   - 回收动作幂等、可审计。
3. GPU session/slot recovery 延续 fence + quarantine 原则：
   lease 过期不等于 GPU 工作完成；未满足 fence 的 slot 不重用。
4. 定义故障级别：
   - graceful shutdown：允许 drain；
   - SIGTERM：有界 quiesce；
   - SIGKILL/OOM：外部检测与下一进程恢复；
   - host reboot：OS 资源消失与持久化状态重建。

故障注入：

- Writer/Reader crash；
- callback 中 SIGKILL；
- container restart；
- RouDi/participant restart；
- OOM kill；
- host reboot 等价测试；
- stale SHM、半写 block、旧 epoch ACK、迟到 GPU completion。

验收：

- 残留资源可检测；
- 每类资源有明确的 safe-to-reclaim 判据；
- 新进程不会消费旧 epoch 数据；
- 恢复时间和不可恢复原因可观测。

### Phase 4：Compatibility Contract

交付：

1. 发布兼容矩阵：
   - N/N-1 C++ API/source compatibility；
   - wire 新旧节点双向通信；
   - metrics schema reader compatibility；
   - record format forward/backward policy；
   - plugin ABI 是否稳定。
2. 所有跨进程 metadata 使用 version + length + feature bits；
   未识别字段可跳过，能力通过 negotiation 协商。
3. Golden artifacts：
   - N-1 生成的 message/record/metrics；
   - N 读取；
   - N 生成、N-1 在声明支持范围内读取。
4. release manifest 固化 Git SHA、release version、依赖 lock digest、
   Fast DDS/Fast CDR patch identity 和 build profile。

验收：

- 新旧节点双向互通；
- 不支持能力时显式降级而非静默伪成功；
- package/wheel/module/tag/runtime version 完全一致；
- lockfile 与 release artifact 可追溯。

### Phase 5：低侵入 Observability v2

当前 Runtime Metrics 已做到默认关闭、固定桶、有界序列和后台 snapshot，
但启用路径仍有 Endpoint/Histogram mutex、shared_ptr 操作和逐消息时钟读取。
它是正确性基线，不应直接称为最终高性能形态。

若固定 runner 上的 contention/尾延迟超出事先制定的预算，
先缩小现有 queue 临界区内的指标工作；仍不能达标时再试点以下目标热路径：

```text
disabled: predictable branch -> return
enabled:  thread-local/per-CPU shard
          -> relaxed counters / bounded histogram
          -> periodic aggregator
          -> immutable snapshot
```

约束：

- 不在逐消息路径构造 protobuf/JSON/string；
- 不在逐消息路径注册 series 或扫描 registry；
- 不允许无界标签、无界样本、无界 retired history；
- histogram 固定内存，可配置采样，明确误差；
- callback/queue identity 在初始化时解析；
- exporter 与业务 transport 隔离；
- 关闭时不得分配 endpoint、读取时钟或复制指标句柄。

性能门禁应覆盖：

- off/basic/detailed；
- 1 writer/1 reader、fanin、fanout；
- 小消息高频、大 payload、慢消费者；
- CPU、cycles/message、allocation/message、lock contention、p99/p999、RSS；
- 固定 runner、CPU affinity、重复轮次和置信区间。

建议目标需要先由稳定 runner 校准，不能将共享开发机结果作为已通过门槛。
首轮可把“off 相对无埋点不可测回归、basic 开销可控、detailed 可采样降级”
作为方向，而不是先写未经验证的百分比。

### Phase 6：Deterministic Runtime

定义“确定性”的适用边界，不承诺通用 Linux 上绝对实时：

- 声明 CPU、内存、队列、线程、时钟和 transport 前提；
- admission control：资源不足时初始化失败，而不是运行中随机退化；
- bounded queue/backpressure；
- scheduler budget、deadline、overrun；
- monotonic/source/synchronized clock domain；
- deterministic replay 所需的 sequence、epoch 和配置快照；
- 优先级反转、锁竞争和分配热点可诊断。

验收使用最坏情况分布和 deadline miss rate，不只比较平均值。

### Phase 7：Devtools 与运维面

基于稳定 schema 提供：

- `monitor`：stale-aware topology、rate、queue、drop、lifecycle state；
- `profiler`：阶段 latency、scheduler wait、callback overrun、contention；
- `diagnostics`：契约能力、版本矩阵、残留资源、recovery reason；
- `doctor`：只读检查为默认，修复/回收需显式确认和审计。

工具不得订阅所有业务 payload 来实现自观测，也不得把失联显示为 0。

## 5. 推荐实施顺序

| 顺序 | 交付 | 原因 |
| --- | --- | --- |
| 1 | 版本一致性检查 + Semantic Contract 文档/characterization tests | 风险高、改动小，立即阻止继续漂移 |
| 2 | Writer structured result + backend capability table | 消除“成功即送达”的根本歧义 |
| 3 | Reader MessageContext + queue/shutdown contract | 为跨进程时间、排序和恢复提供身份 |
| 4 | 资源所有权状态机与顺序验证；必要时试点 Coordinator | 在更多后端埋点前稳定资源所有权，不预先引入通用依赖图 |
| 5 | crash/restart fault matrix 和 SHM/GPU recovery | 把可恢复从口号变为可验证能力 |
| 6 | Compatibility matrix 和版本化 metadata | 为跨进程 metrics 与新旧节点互通打底 |
| 7 | Metrics 固定 runner 门禁；预算失败后才考虑分片聚合 | 在语义稳定后按实测瓶颈优化正确的指标 |
| 8 | deterministic budgets + devtools | 消费稳定契约，而不是猜测内部状态 |

Runtime Metrics 原计划中的后端分段和跨进程关联应分别放在 Phase 1/4
契约确定之后；否则会把不稳定语义固化进 schema。

## 6. 完成定义

不得仅以“测试通过”或“性能更快”宣称 Runtime 演进完成。完成至少要求：

- 每个公开 API 的成功、所有权、顺序、时间和 shutdown 语义可查；
- 每个跨进程资源有 owner、epoch、stale 和 safe-reclaim 判据；
- graceful、SIGTERM、SIGKILL/OOM、restart 的保证边界明确；
- tag/module/deb/wheel/runtime/manifest 版本一致；
- 新旧版本兼容矩阵有自动化证据；
- metrics 在关闭和开启模式均有稳定 runner 的资源与尾延迟预算；
- 诊断工具能区分 zero、N/A、unsupported、stale 和 failed；
- 所有恢复动作可审计，不以“删除残留文件”冒充安全恢复。
