# Runtime Metrics 实施计划

状态：分阶段实施中；本文保留完整目标，当前可用范围见
`runtime-metrics-usage.md`，性能测量方法与局限见
`runtime-metrics-performance.md`。Runtime 的 Semantic、Lifecycle、
Compatibility 与 Recovery Contract 总体优先级和依赖关系见
`../roadmaps/runtime-contract-evolution.md`。基于 2026-09-28 当前工作区代码检查。

已落地本地基础切片：默认关闭的固定内存指标库、配置、进程 JSON 快照、
逻辑发布/共享接收计数、消费者游标队列的积压/溢出/取出计数，
Reader 与 Component 回调计时，以及逻辑发布 API 耗时。
队列、融合与回调已有单测，并加入 `//tests/integration_test:runtime_metrics_test`
覆盖共享 Receiver、慢回调积压与丢弃（纳入 core_tool_matrix_tests）。
速率已改为最近 60 个一秒桶的固定分母滚动估计，跨整分钟边界不断档；
分位数仍为 60 秒对齐窗口、不是目标的滚动窗口，
关闭时没有跨进程指标。新增
`//tests/perf_test:runtime_metrics_hot_path_benchmark` (off/basic/detailed
独立进程 JSON) 及输出/注册 churn 单测；注册表只在满时回收已过期弱句柄，
避免逐次注册扫描。这个合成 microbenchmark 仅测指标路径本身，
不替代真实 pub/sub 的性能门禁。现有单进程 Node pub/sub 基准及
多轮交错顺序汇总可验证模式生效、全部预热/测量交付，并保留原始样本；
它仍不是固定 runner 的端到端验收。PR3a 已接入任务通知至 Resume 的
独立调度分布和累计次数（含合并通知单测、真实调度及受阻 Processor
集成测试），不将它
与 queue_latency 相加；PR2 在 buffer 销毁时按消费者游标统计可读取
未处理项为 shutdown discard（含空队列、首次 Fetch 策略和复制防重单测）；
真实 Reader 与多输入 Component 融合队列的关停、in-flight 回调/Proc
生命周期已由集成测试验收。队列销毁后的最终 endpoint 快照保留在最多
4096 项的有界历史中，超出时增加 `retired_snapshots_dropped`；
该复制和注册表锁只发生在队列销毁路径，不进入消息热路径。消息级跨进程身份为待办。尚未完成
PR3b 后端阶段、PR4 兼容的跨进程关联、PR5 cyber_monitor 专用视图、
PR6 稳定 runner 性能门禁。

完成标准：PR2 剩余消息身份与生命周期竞态、PR3b 实际后端分段、
PR4 新旧节点双向互通和可信时钟、PR5 stale-aware 运维视图
以及 PR6 固定机器多轮验收均有相邻单测、`tests/` 集成测试和可复现
证据；缺任一项不得称为全链路可测或达到性能预算。
“SOTA”不作为无基准的完成声明，以本计划的准确性、资源上限和
指定负载下的可重复对照为验收依据。

### 2026-09-30 可观测性复核与执行范围

业界对照按**不同观测层级**评价，不把功能名称相近当作同等能力：

| 基线 | 可核查的能力 | Core 1.1 当前边界 |
| --- | --- | --- |
| [ROS 2 Topic Statistics](https://docs.ros.org/en/humble/Tutorials/Advanced/Topic-Statistics-Tutorial/Topic-Statistics-Tutorial.html) | 订阅侧 message age/period 的窗口均值、极值、标准差和样本量 | Core 进一步区分本地队列积压、丢弃原因与回调尾延迟；没有可信的跨进程 message age |
| [Fast DDS 2.14 Statistics](https://fast-dds.docs.eprosima.com/en/v2.14.7/fastdds/statistics/dds_layer/statistics_dds_layer.html) | DDS 层网络/历史延迟、吞吐、RTPS 丢失及发现 | 互补而非替代；Core 当前没有 DDS 后端分段或传输丢失归因 |
| [OpenTelemetry Metrics SDK](https://opentelemetry.io/docs/specs/otel/metrics/sdk/) 和 [Prometheus Histograms](https://prometheus.io/docs/practices/histograms/) | 有界基数、溢出保总量、可合并直方图桶与标准采集生态 | Core 有 4096 活跃序列上限及拒绝计数，但拒绝后新增端点的事件不计入总量；仅导出分位数，不能跨端点重算 p99 |

本轮只改进已有进程内诊断，不改变消息/Writer/Reader API、传输协议或
默认关闭策略：

1. **准确性**：退役端点标明状态与时间，不将其历史窗口冒充当前值；
   跨窗口队列峰值包含仍在队列里的积压；单样本分位数不能超过精确 max。
   多输入 DataVisitor 先安装融合回调和指标再向 dispatcher 注册主输入，
   融合对象析构前等待已开始的主输入 Fill 完成并解除回调，避免关停后
   访问已销毁的融合队列。相邻单测与 Reader/Component 关停集成测试覆盖
   队列不变量和析构后的主输入访问。
2. **有界快照成本**：四个分位数共用一次固定桶扫描，保持原 nearest-rank
   和桶估计口径；单测覆盖同桶、分离的尾桶、上下界与窗口切换。
   最坏仍与活跃直方图数及每直方图 834 个桶成正比，不能凭算法复杂度
   推断实际开销已经满足目标。
3. **性能门禁（未验收前保持默认关闭）**：固定 runner、冻结硬件/配置、
   交错运行 off/basic/detailed，核对全量交付及模式生效；单列
   Snapshot/JSON/文件导出在多消费者和 4096 序列附近的 CPU、RSS、
   p99 与导出周期，另外验收真实 Node fanout 和慢消费者热路径。
   使用同环境基线和预先约定的预算；共享开发机或限速单进程样本只能
   用于诊断，不能证明“业界最优”或默认开启安全。
   已添加 `//tests/perf_test:runtime_metrics_snapshot_benchmark` 的
   输出契约测试和独立进程对照入口；首批 4096 活跃端点样本的 JSON
   约 10.76 MB，若逐秒持续写入约 38.7 GB/小时（未测文件写入）。
   已确认 Core 1.1 保持现有 opt-in 的每秒导出语义及 4096 序列上限；
   优先做固定 runner 验收，超出事先约定的预算后再决定是否调整策略，
   不以放宽刷新时效掩盖当前成本。

如需 Topic 聚合，应先定义可合并的桶、窗口/标签语义和溢出时的全局
总量口径，并用反例测试证明准确性；不要平均端点 p99，也不要把
RTPS loss、本地 queue drop 与 shutdown discard 相加。跨进程关联和
后端阶段留在 Core 1.1 之后的兼容性方案内。

提交前复核：融合对象先安装回调再向 dispatcher 注册主输入，
析构时同步解除捕获自身的回调；2/3/4 输入均有销毁后访问回归测试。
单测、融合和组件测试、Metrics 性能输出契约已加入
`scripts/release/ubuntu2204_baseline.sh`。本地 Ubuntu 基线 36/36 通过
（最近一次 35 项使用 Bazel 缓存；此前 33/33 完整执行，新增的 3 项
独立运行过）。这不是全新容器或固定 runner 性能验收，不能据此
解除 Metrics 默认关闭或声称正式发布资格。

### 下一步（按风险排序）

1. **口径与开销门禁**：合成热路径 off/basic/detailed 对照和输出测试
   已落地。现有 `cyber_rt_benchmark_suite` 绕过 Node 埋点，不适合
   作为本指标开销对照；新增单进程真实 Node pub/sub 对照和三模式
   输出验收及交错顺序的多轮原始 JSON 汇总（见性能测量文档）；
   预热全部交付后才开始测量，测量阶段单列发送与排空时间。
   丢包或模式不符不生成统计结论。下一步在固定机器扩展跨进程与
   多消费者负载，对照吞吐、CPU、p99 和内存上限，验明各子进程
   模式生效后再重复评估波动。
   不用合成 `ns_per_iteration` 证明端到端门槛，仅报告实测值，
   不将建议阈值写成已通过结果。
   当前对齐窗口分位数已标出单调时钟窗口起止及样本量；若要换成滚动窗口，
   先提交固定内存预算和误差测试，不能存储全部消息样本。
2. **任务调度后续校准（PR3a）**：现有实现只在 DATA_WAIT/IO_WAIT
   收到首次通知时记时；持续 READY、SLEEP 自动到期及首次运行不采样，
   不提供消息级调度归因。受阻单 Processor 与合并通知已有测试，
   已验证停止前未 Resume 的任务不产生成功样本；下一步在固定 runner
   上校准指标自身开销，并进一步覆盖两种策略的移除/通知竞争。
3. **关闭时积压口径（PR2）**：基础计数已在队列析构按可读游标记录；
   Reader 与多输入 Component shutdown 集成已通过：在回调/Proc 阻塞时
   确认积压，验证 shutdown 等待执行中的协程完成后才退役队列、未消费
   数量准确、inflight 清零且最终快照仍可见。仍需在后续并发/重启压力
   测试中覆盖消息身份与缓冲区复用竞态。
4. **后端分段（PR3b）**：每个实际启用的后端分别测发送准备、
   acquire、序列化、提交和后端失败；SHM/RTPS/ICEORYX、
   HYBRID 部分失败及 Loan 路径分别测试。不得把 `Write()` 成功
   当所有订阅者送达，零拷贝没有序列化阶段则输出 N/A。
5. **跨进程关联（PR4）**：先设计版本化可选元数据并证明新旧节点
   双向互通与时钟域有效，再接入 transport/end-to-end；
   时钟信息不足保持 `not_instrumented`，不以消息体时间戳冒充。
6. **运维消费（PR5）**：解析快照 schema、校验 `process_instance` 与
   `generated_at_unix_ns` 的存活性，按 Topic 聚合 histogram 原始桶
   而非平均 p99；无导出/过期/版本不兼容应显式展示原因。

## 1. 目标与交付顺序

将 core 从“能查看拓扑和收发消息”提升为“能定位运行时瓶颈”。
第一优先级不是新增传输后端，而是回答：
某个订阅者变慢，是发送侧、传输、消费积压、调度，还是业务回调造成的？

交付顺序：

1. 本地可测量闭环：发布/接收/消费计数、真实待处理队列、丢弃原因、
   队列等待、回调耗时和分位数，可通过进程快照读取。
2. 调度诊断与跨进程关联：补齐发送阶段、传输延迟、端到端延迟，
   明确时钟有效性与新旧节点兼容。
3. 运维入口：cyber_monitor 展示目标进程指标，支持机器读取和外部采集。

首版不改业务消息 schema、回调签名、队列策略、调度策略或 QoS，
不要求接入 Prometheus / OpenTelemetry，不升级 Fast DDS。
“不可测量”必须显示 N/A 及原因，不能以 0 或平均值代替。

## 2. 当前代码结论

| 位置 | 已有能力 / 行为 | 对计划的影响 |
| --- | --- | --- |
| `cyber/event/perf_event.h`、`perf_event_cache.cc:68-143` | 定义了 transport/scheduler 事件，异步输出文件；启用后每事件分配对象，队列入队结果未用于统计 | 可保留为离线诊断入口，不直接充当常开 metrics 后端 |
| `cyber/node/reader_base.h:202-226` | Receiver 按 channel 复用；拿到 MessageInfo 后，Dispatch 只传消息指针 | 消息身份和计时上下文在进入队列前中断 |
| `cyber/data/cache_buffer.h:49-82`、`channel_buffer.h:57-80` | 环形缓存不会因消费弹出；首次 Fetch 读最新，落后溢出时也跳到最新 | Size 不是 queue_depth；覆盖历史数据不必然是丢弃未处理消息 |
| `cyber/data/data_visitor.h:190-196` | 消费进度由 next_msg_index 表示 | queue_depth / drop 必须结合消费者游标统计 |
| `cyber/node/reader.h:244-249,258-280,361-378` | Enqueue 在调度任务中执行；GetDelayNs 表示消息年龄/间隔一类量 | 不能将 GetDelayNs 当成 transport 或 end-to-end latency |
| `cyber/component/component.h:177-219,246-315` | reality 模式下，Reader 和 Component 的 DataVisitor/任务是不同路径 | 仅包 Reader 回调会漏掉 Proc；内部缓存任务不能重复计为业务回调 |
| `cyber/data/fusion/all_latest.h:40-85` | 主输入触发融合，辅输入取最新；缺少辅输入时不形成融合输出 | 主触发队列、融合跳过、辅输入年龄需要不同口径 |
| `cyber/scheduler/processor.cc:38-60`、`scheduler.cc:112-135` | 有当前执行任务快照，但没有消息级等待分布 | 需在任务 ready / Resume 边界补调度诊断 |
| `cyber/transport/message/message_info.cc:27,66-109` | MessageInfo 固定长度，反序列化严格检查长度，没有发送时间 | 不能直接加时间戳并假定旧节点兼容 |
| `cyber/transport/transmitter/rtps_transmitter.h:100-125` | 消息身份通过 RTPS related_sample_identity 传递 | 只修改 MessageInfo 序列化不足以覆盖 RTPS |
| `cyber/transport/transmitter/hybrid_transmitter.h:235-264` | 无活动后端时返回 true；多个后端中任一个成功也返回 true | API 成功、各后端成功、订阅者接收必须分开 |
| `cyber/transport/shm/profile.h:38-72` | 有样本数、最大 payload、write_busy，输出 TOML | 复用相关观测点，保留 profile 兼容；不复制其全局锁到新指标热路径 |
| `cyber/tools/cyber_monitor/general_channel_message.cc:177-221` | frame_ratio 来自 monitor 自身订阅 | 无法反映目标业务 Reader 的队列和回调 |
| `tests/perf_test/benchmark_sub.cc:507-521` | benchmark 已计算 p50/p95/p99/p999/max | 可作为离线参考验证；不能将收集全部样本后排序搬进常驻 Runtime |

当前源码中的 AddSchedEvent 没有发现实际调用点；transport 事件调用主要在
发送开始和接收 Dispatch/Notify。事件枚举齐全不等于埋点链路齐全。

## 3. 指标口径

### 3.1 身份与聚合

基础身份：`host_instance / process_instance / node / channel / endpoint`。
process_instance 必须能区分进程重启，不能只用 PID。
队列额外带 `consumer_id / queue_kind`，执行指标带 `task/component`；
传输阶段带实际后端 INTRA、SHM、RTPS、ICEORYX。

默认按 Topic 展示，再按 Reader/Component 下钻。发布者只计一次逻辑发布，
共享 Receiver 的物理接收和每个消费者的投递分别计数。
一发多收时 receive_count 大于 publish_count 是正常现象。
消息序号仅用于内部关联，不能成为 metrics label。

| 指标 | 定义 |
| --- | --- |
| `publish_count` | Writer::Write / Publish 返回成功的次数，表示 API 接受，不表示送达 |
| `publish_attempt_count` / `publish_error_count` | 逻辑发布调用次数 / 返回失败次数；Loan 不算发布 |
| `receive_count` | Receiver 完成解码/借用并接受的消息数，每个共享 Receiver 计一次 |
| `enqueue_count` / `dequeue_count` | 指定消费者工作队列的逻辑入队 / 成功取出次数 |
| `drop_count{reason}` | 已确定未消费消息的丢弃，按消费者、队列和原因分开 |
| `queue_depth` | 此消费者队列内仍可取出的未处理条目数，不含正在执行的回调 |
| `queue_high_watermark` | 同一 queue_depth 的生命周期峰值；另提供统计窗口峰值 |
| `publish_rate` / `receive_rate` | 同一身份的计数增量 / 单调时间窗口；标明窗口长度 |
| `callback_count` | 实际进入业务回调 / Proc 的次数，不含纯内部缓存转发 |
| `callback_error_count{reason}` | 可观察的失败：Proc 返回 false、逃逸异常、显式报告的业务错误 |
| `callback_completed_count` / `callback_inflight` | 已结束回调数 / 正在执行回调数，辅助识别长期不返回 |

普通 Reader 回调返回 void，不能自动推断业务失败。记录逃逸异常时保持原有异常
传播/终止语义，不吞异常继续执行；崩溃前的最终快照只能 best-effort。
Component 返回 false 与 shutdown 提前返回不能混为一类。

drop 的第一批原因：`overflow_overwrite`、`overflow_skip_to_latest`、
`initial_skip_to_latest`、`shutdown_discard`。
覆盖和 Fetch 跳过按不相交的序号区间记账，不能把同一消息记两次。
已消费的历史条目被覆盖不算 drop。
融合缺少辅输入单列 `fusion_skip_count{missing_input}`；
辅输入最新值被替换不自动当作业务丢包。

发送失败、反序列化失败、SHM busy、疑似序号缺口是不同层级的诊断指标，
不能加成同一个 drop_total。序号缺口受重启、乱序、历史重放、订阅时刻影响，
只显示为 `sequence_gap_count`，不冒充确定的传输丢失数。

### 3.2 时间边界

定义：

```text
t0 publish API enter
t1 transport handoff / visibility boundary
t2 receiver decoded / borrowed message ready
t3 consumer queue enqueue
t4 consumer dequeue / successful TryFetch
t5 user callback enter
t6 user callback exit
```

| 指标 | 边界与说明 |
| --- | --- |
| `publish_latency` | Write/Publish 入口至返回；API 阻塞时间，不作为可加和阶段 |
| `publish_prepare_latency` | t1 - t0，至所选传输分支交付前的准备；下列序列化/SHM 子阶段仅用于解释它，不再次相加 |
| `serialization_latency` | 各后端实际序列化开始至结束；零拷贝无此阶段时标 N/A |
| `shm_acquire_latency` / `shm_write_latency` | 等待可写块 / 实际写入提交，和序列化避免重叠记账 |
| `transport_latency` | t2 - t1，含传输至接收解码完成；明确后端具体 handoff 位置 |
| `dispatch_latency` | t3 - t2，按目标消费者分别统计 |
| `queue_latency` | t4 - t3，含消费者积压及其中的调度等待 |
| `callback_dispatch_latency` | t5 - t4，框架从取出到业务入口的成本 |
| `callback_latency` | t6 - t5，墙上耗时，包含回调内阻塞，不宣称为纯 CPU 时间 |
| `end_to_end_latency` | t6 - t0，单条发布至指定消费者回调结束，不是整个 AD DAG |
| `scheduling_latency` | task 首次具备运行条件至下一次 Resume，独立 task 诊断指标 |

INTRA 同步交付时接收可能早于 Transmit 返回，t1 必须在调用下游前；
SHM/RTPS 也不能拿发送 API 返回时间冒充可见边界。
并行 HYBRID 分支分别计时，不能将各分支串行相加。
Loan 到 Publish 之前的用户填充时间不在端到端定义内。

**禁止将 queue_latency 与 scheduling_latency 直接相加。**
当前是协程轮询/通知模型，Notify 与消息不是一一对应，回调运行中也会收到消息。
MVP 展示包含调度影响的 queue_latency，辅以 task 调度分布。
后续采样 trace 只有能证明消息可执行边界时，才拆成互斥的 backlog / runnable wait；
否则保留综合等待，不能伪造“70 ms 队列 + 10 ms 调度”。
所有阶段 p99 也不能相加推导端到端 p99；端到端必须独立采样。

本地持续时间使用 Time::MonoTime（steady_clock），与业务/仿真时钟隔离。
跨进程不能依据 C++ steady_clock 类型直接假定 epoch 相同：
同主机需验证相同 clock domain、boot/time namespace；跨主机需同步时钟、
同步状态和误差界。时间源未知、旧节点无时间戳、时钟跳变时输出 N/A + reason，
并增加 invalid/unavailable sample 计数，不把负数截成 0。

### 3.3 分布、窗口和开销

每个 latency 输出 `count / p50 / p90 / p99 / p999 / max`，附带
`window / sampled_count / sampling_ratio / validity`。
目标固定 60 秒滚动窗口，并保留累计计数和生命周期 max。
max 单独保存实际观测值，不能用 histogram 桶上界代替。

采用固定内存、可合并的直方图；实现前比较固定对数桶与已有兼容实现，
不新增大型 telemetry 依赖。建议目标：1 us 至 60 s 范围内相对桶误差 <= 2%，
明确零值、underflow、overflow；超范围分位数显示范围受限。
使用确定性样本分布验证误差，而不是仅验证字段存在。

聚合时合并相同 schema、相同窗口的桶，不能平均各 Reader 的 p99。
Topic 视图保留最慢 Reader 和最大单队列积压，不只展示总量。
10 Hz 的 60 秒窗口仅约 600 个样本，p999 不能作为可靠尾延迟结论；
显示样本不足告警，并支持离线合并窗口，不把经验分位数当作置信保证。

指标分级为 off / basic / detailed，首发默认 off；basic 提供计数、
queue、queue/callback 分布，detailed 增加阶段和采样 trace。
句柄在注册期解析，热路径不得按消息查询字符串表、输出日志/文件或分配样本对象。
计数保持全量；如耗时采样，关联阶段使用相同消息采样决策。
必须有进程级内存/series 上限；达到上限显式告警和自监控，不能静默丢指标。

## 4. 分 PR 实施

| PR | 交付内容 | 主要改动面 | 退出标准 |
| --- | --- | --- | --- |
| 1：契约与底座 | 指标 schema、时钟封装、计数器/直方图、注册与快照 API、配置分级 | 新 `cyber/metrics/` 及本地 BUILD/tests；`cyber/proto/cyber_conf.proto`、配置示例、`cyber/init.cc` | 空窗口 N/A；分位数误差可验证；并发快照和注册注销安全；有开销基线 |
| 2：本地消费链路 | 传递本地 MeasurementContext；队列游标记账；Reader/Component 回调指标；逻辑发布与物理接收计数 | `node/writer.h`、`node/reader*.h`、`data/*`、`data/fusion/*`、`croutine/routine_factory.h`、`component/component.h` | 慢消费者可同时看到积压、准确 drops、queue/callback 分布；不改 Fetch/AllLatest 行为 |
| 3：调度与后端阶段 | task ready-to-resume；序列化、SHM acquire/write、后端错误和实际路径 | `scheduler/processor.cc`、两种 scheduler policy、`croutine/`、transport transmitter/receiver/dispatcher | 能区分调度饥饿和慢回调；通知合并、持续 READY、IO_WAIT 明确分开；后端失败不被 API 成功掩盖 |
| 4：跨进程关联 | 版本化可选计时元数据、时钟有效性、跨进程 transport/E2E 分布 | `transport/message/`、SHM/ICEORYX 元数据、RTPS underlay/适配层、HYBRID/history | 新旧节点双向兼容；无支持时仍正常交付并显示 N/A；跨时钟域拒绝无依据计算 |
| 5：运维入口 | 周期原子快照、cyber_monitor Topic/Reader 视图、JSON 导出、采集文档 | `cyber/metrics/`、`cyber/tools/cyber_monitor/`、相邻 BUILD、工具集成测试 | 不订阅业务 payload 也可读目标进程指标；失联显示 stale 而非 0；快照失败可见 |
| 6：发布门禁 | 完整故障注入矩阵、开销/持续运行报告、配置与排障文档 | 邻近单测、`tests/integration_test/`、`tests/perf_test/` | 原行为回归通过；准确性与性能预算通过；明确支持范围及未覆盖项 |

依赖：PR1 -> PR2；PR3 依赖 PR2；PR4 依赖 PR2/3。
PR5 的本地视图可在 PR2 后并行推进，不必等 PR4。
测试随每个 PR 提交，PR6 是最终整体验收，不是到最后才补测试。

### 实现约束

- MeasurementContext 至少包含 writer identity、sequence、来源实例、
  本地到达时间和计时有效性。作为内部队列条目的一部分与消息同生共灭，
  不修改业务对象，不用裸指针全局 map 猜测身份；相同 shared_ptr 再次发布也应区分。
- 元数据与 payload 在同一个队列同步边界内写入/读取，溢出、融合、
  HYBRID 历史补发都须保持配对；旧 Dispatch 调用保留无来源上下文路径。
- 队列状态与消费者游标共同维护；禁止在 Registry 中额外持锁再进入队列锁。
  snapshot 不应阻塞业务等待 exporter。
- Component 的业务 Proc 只记一次；reality 与非 reality 模式统一业务指标语义。
  多输入组件以主输入计触发 E2E，辅输入提供 input_age，不能把一次 Proc
  当成每个辅输入的新回调。
- Reader 无回调/Observe 历史缓存用 queue_kind 区分；Blocker history depth
  不是待执行工作队列。TimerComponent 的耗时属于 task，不伪造 channel。
- wire 扩展必须先提交兼容方案：版本、能力发现、字节布局、旧读取器行为、
  mixed-version HYBRID fanout。旧 SHM kSize 和 RTPS sample identity 不可被随意复用。
  优先评估可选扩展，无法安全扩展则使用明确版本化路径；不偷偷改旧载荷。
- 导出第一版建议独立后台线程在配置的私有 runtime 目录生成进程实例快照，
  临时文件 + 原子替换，携带 schema_version、窗口和刷新时间；本地 monitor 读取。
  不经被测 Cyber channel 发送 metrics，防止自观测和故障耦合。
  跨主机汇总由外部采集器完成；HTTP/Prometheus adapter 可后续添加。
- 初始化在工作线程使用句柄前完成；退出时先停止指标生产者，再停止/刷出 exporter，
  保持既有 scheduler/transport/topology 清理相对顺序，覆盖 Python 特殊退出路径。

## 5. 验收计划

### 准确性与故障注入

| 场景 | 必须验证 |
| --- | --- |
| 单 Writer / 单 Reader，已发现后发送 N 条并排空 | publish、physical receive、enqueue/dequeue、callback 精确计数；queue_depth 最终为 0 |
| 同进程多个 Reader、跨进程 fanout、fanin | 接收层与消费层不重复；不同 Writer 同序号不串样本 |
| 已消费历史被覆盖 | drop 不增加，queue_depth 不因历史缓存满而保持满 |
| 首次 Fetch 前突发、慢消费者、溢出跳最新 | 分别核对 overwrite / policy skip，精确匹配实际交付序列；不能只测总数大于 0 |
| 队列内持续积压但暂不消费 | depth/high watermark/drop 随生产侧变化，不等下一次 Fetch 才全部更新 |
| 固定耗时回调、阻塞回调、持续 READY 任务 | queue/callback 与 scheduling 分布区别明确；inflight 可见 |
| Proc false、Reader 异常、组件 shutdown | 错误分类准确；不改变异常传播、不将未执行 Proc 算成功回调 |
| 2/3/4 输入 AllLatest | 缺输入跳过、辅输入复用、融合队列溢出可区分，数据与 metadata 配对 |
| INTRA/SHM/RTPS/HYBRID/ICEORYX，普通/loan 发布 | 无多次发布计数；实际后端与阶段准确；无序列化阶段不造数 |
| 空闲、重启、counter reset、退出和 exporter 故障 | rate 不沿用旧进程数据；无样本 N/A；stale/错误明确；内存释放/上限行为可验证 |
| 旧节点、不同时间域、时钟回拨/不同步 | 保持消息交付，不产生错误的负延迟或虚假 0 延迟 |
| 已知样本分布及窗口轮换 | p50/p90/p99/p999 误差、max、窗口边界、并发合并均可重复验证 |

纯时间逻辑用可注入单调时钟做确定性测试。真实线程/跨进程测试使用
discovery-gated 启动、完成屏障和有界超时，不用固定 sleep 猜测连接完成。
对实际延迟归因用明显可分辨的故障注入及相对比较，避免脆弱的精确毫秒断言。

### 相邻测试入口

按修改范围选择执行，不要求每个 PR 都跑全套：

```bash
bazel test //cyber/data:cache_buffer_test \
  //cyber/data:channel_buffer_test //cyber/data:data_dispatcher_test \
  //cyber/data:data_visitor_test //cyber/data:all_latest_test \
  //cyber/node:reader_test //cyber/node:writer_test \
  //cyber/node:writer_reader_test //cyber/component:component_test \
  //cyber/scheduler:scheduler_classic_test \
  //cyber/scheduler:scheduler_choreo_test \
  //cyber/scheduler:processor_test --test_output=errors

bazel test //tests/integration_test:examples_regression_tests \
  //cyber/transport/integration_test:rtps_transceiver_test \
  //cyber/transport/rtps:rtps_test \
  //tests/integration_test:core_tool_matrix_tests --test_output=errors
```

新 metrics 单测与库放在 `cyber/metrics/BUILD`；集成场景放入相邻集成 package。
现有 `//tests/perf_test:cyber_rt_benchmark_suite` 及辅助程序直接访问
transport，不经过 Node 指标路径；不能用它验证本次 Runtime Metrics 的开销。
先用 `//tests/perf_test:runtime_metrics_pubsub_benchmark` 对照单进程
Node 路径，跨进程/多消费者稳定 runner 门禁仍待补齐。

### 建议性能门槛

以下是待 benchmark 校准的验收目标，不是当前测得结果：

- off：吞吐和 p99 延迟相对基线退化 <= 1%；先验证平台噪声是否可分辨此阈值。
- basic：吞吐退化 <= 5%，p99 延迟增长 <= 5%，CPU 相对增长 <= 5%。
- detailed：测量并公布不同采样率成本，不以无限制全量 trace 作为生产默认。
- 固定内存预算，覆盖 series 上限、Topic churn 和至少 2 小时持续运行；
  不得随总消息数增长，不因采集端离线阻塞业务。

固定机器、CPU 亲和性和负载条件，多次重复，报告波动/置信区间；
小消息高频、大 payload、慢消费者和 fanout 均需覆盖。
共享 CI 只做准确性与低门槛回归，严格性能门槛在稳定 runner 执行。

## 6. 范围边界与首个完成里程碑

首个里程碑为 PR1 + PR2 + PR5 的本地快照部分：
在不修改 wire format 的条件下，从目标业务进程直接看到每个消费者的
queue_depth/high_watermark/drop、queue latency 和 callback latency 的尾部分布，
并通过慢消费者案例证明能定位积压来源。

这时跨进程 transport/E2E 若无可信来源时间仍显示 N/A；
不能称为全链路完成。完整目标在 PR4 和整体验收后才完成。

Python 用户函数、RPC request/response、Timer、GPU/NvSci 的实际执行边界
需要各自适配与专门测试；首版若仅观测到 C++ 桥接回调，必须明确标注，
不得冒充 Python 用户函数/GPU kernel 完成时间。
完整 AD DAG 的跨 Topic 因果链追踪、GPU fence/device 时间和 SLO 告警策略
作为后续扩展，不混入本次通用 pub/sub 指标底座。
