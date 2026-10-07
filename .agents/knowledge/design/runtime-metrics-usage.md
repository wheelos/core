# Runtime Metrics 当前能力

当前实现是 Runtime Metrics 的**本地诊断切片**，不是完整跨进程端到端
跟踪。默认关闭；不会更改业务消息、传输协议、队列策略或回调签名。

在 `cyber/conf/cyber.pb.conf` 中按需加入：

```protobuf
runtime_metrics_conf {
    mode: BASIC
    snapshot_path: "/run/user/1000/cyber-metrics"
}
```

`snapshot_path` 是绝对路径前缀，父目录必须由运行用户拥有且不能对 group/other
开放；进程周期性写入 `<prefix>.<pid>.json`，原子替换、文件模式 0600。
启动时不能导出会报错并使 `cyber::Init` 失败，运行中导出失败会输出错误。
`snapshot_path` 留空时只支持进程内
`metrics::Registry::Instance().Snapshot().ToJson()`；没有外部进程快照。
`BASIC` 与 `DETAILED` 目前具有相同指标范围，DETAILED
尚不代表跨进程 trace。关闭的进程不会注册指标。
最多注册 4096 个同时存活的指标序列；超出限制会写错误日志、
递增 `rejected_registrations`，并使该端点的指标不可用，不影响业务收发。
启用每秒 JSON 导出前，应按活跃端点数量核对快照大小和写入预算；
4096 只是数量上限，不代表端侧周期导出的 CPU、RSS 或磁盘预算已通过。

快照按 writer、共享 receiver、consumer 和 task 分项，记录：

- 成功/失败的逻辑 Write/Publish、接收、消费者入队/出队、回调次数与错误；
- `publish_latency`：Write/Publish 调用直到返回的耗时（成功和失败均记录），
  包括复制/序列化和 API 内阻塞，不是发送到接收的传输延迟；
- 按消费者的待处理深度、高水位和丢弃原因；
- 60 秒**对齐窗口**内的 publish/queue/callback latency
  p50/p90/p99/p999/max；发布和接收速率按最近 60 个一秒桶计数除以
  60 秒（不足 60 秒时仍按 60 秒除，不推算尚未发生的流量）。
  速率约有一秒边界误差，计数器为累计值。
- `queue_window_high_watermark` 是当前 60 秒对齐窗口内的最大队列深度；
  跨窗口仍未消费的积压会作为新窗口的初始深度，不能报告低于当前深度的峰值。
  分位数由桶近似计算，不超过精确的 `max_ns`。
- task 序列的 `task` 是协程名称（`channel` 为空），`scheduling_count`
  是累计成功采样次数，`scheduling_latency` 是当前对齐窗口内
  DATA_WAIT/IO_WAIT 首次有效通知到下一次 Resume 的分布；重复通知
  合并为一次。首次运行、持续 READY、SLEEP 自行到期和运行中的
  无效通知不采样；其他序列的此字段为 `not_applicable`。

queue latency 从当前进程消费者队列入队到 Fetch，包含调度等待。
task-level scheduling latency 不属于某条消息，不能与 queue latency
或其他分位数相加以推导 end-to-end latency。
callback latency 从进入 C++ Reader 回调或 Component::Proc 到退出，
包含回调阻塞；`callback_error_count` 计 Proc 返回 false 或逃逸异常，
不自动推断 void 回调中的业务失败。初次 Fetch 使用最新消息，因此
此前未消费条目计入 `initial_skip_to_latest`；环形缓存覆写未消费条目、
溢出 Fetch 跳过仍可消费的中间条目分别计入
`overflow_overwrite` 与 `overflow_skip_to_latest`。
`shutdown_discard` 在 consumer 缓冲区存活期间为 null；缓冲区销毁时
统计仍可读取的未处理条目，并将队列深度置零，之后以数字输出（无积压为 0）。
已覆写或首次 Fetch 策略已跳过的消息不重复计入 shutdown discard。
队列退役时 Registry 会保留最多 4096 个最终 endpoint 快照，使关停后的积压
仍能从进程快照读取；它们不计入 `live_series`。容量耗尽时淘汰最早的退役快照，
并通过 `retired_snapshots_dropped` 暴露累计淘汰数。最终快照仅在队列销毁路径
生成，不增加逐消息热路径工作。每个 endpoint 的 `retired` 标记是否仍在运行；
活跃 endpoint 的 `retired_at_unix_ns` 为 null，退役快照为退役时的 Unix 纳秒时间。
退役快照的速率、窗口峰值和分位数是**历史终态**，即使进程快照仍在更新，也
不能当作当前窗口的指标或与同名活跃 endpoint 合并用于告警。
Component 的多输入队列以主输入融合输出为消费单位。
在 simulation 模式 Component 的内部 Reader 回调也作为单独的 consumer
出现；它与 Component::Proc 的调用次数不可直接相加。

JSON 中未实施的阶段（包括 transport 和 end-to-end latency）标为
`not_instrumented`，没有样本时分位数是 null。
当前还不支持：消息级跨进程身份和可信时钟关联、消息级调度独立归因、
Python 用户函数/GPU 执行时间、完整 Topic 聚合与 cyber_monitor
专用面板。分布采用固定内存对数桶；**延迟分布**是 60 秒对齐窗口，
不是滚动窗口，跨窗口不合并；速率单独采用逐秒滚动桶。
`window_start_mono_ns`/`window_end_mono_ns` 是 steady clock 的纳秒刻度，
只用于解释当前进程的窗口，不得当 Unix 时间或跨机器关联时间。
同一个快照中的累计计数、队列深度和各直方图分别读取，不是跨指标/
跨端点的原子事务；并发收发时不能用一次快照断言它们逐条精确对齐。
空窗口尾分位数没有统计意义；
低样本量 p999 不具备稳定的尾延迟推断价值。进程退出后快照文件仍可能存在，
读取方应校验 `generated_at_unix_ns` 和 `process_instance`，超时标记为 stale。

后续按 `runtime-metrics-plan.md` 中的 PR3-PR6 扩展，不应将未实现阶段的
`not_instrumented` 当作 0 ms。
热路径微基准入口及与真实 pub/sub 门禁的区别见
`runtime-metrics-performance.md`。
