# Runtime Metrics 性能测量

`//tests/perf_test:runtime_metrics_hot_path_benchmark` 是独立的
**指标热路径微基准**：每次迭代为相同的轻量原子操作，basic/detailed
额外记录逻辑发布、接收、队列状态和队列/回调分布。每种模式
**单独运行一个进程**；输出一行 JSON，包括迭代数、墙钟/CPU 纳秒、
每迭代纳秒、峰值 RSS（KiB）和累计计数。

```bash
bazel build //tests/perf_test:runtime_metrics_hot_path_benchmark
source scripts/env/runtime.bash
./bazel-bin/tests/perf_test/runtime_metrics_hot_path_benchmark off 10000 100000
./bazel-bin/tests/perf_test/runtime_metrics_hot_path_benchmark basic 10000 100000
./bazel-bin/tests/perf_test/runtime_metrics_hot_path_benchmark detailed 10000 100000
bazel test //tests/perf_test:runtime_metrics_hot_path_benchmark_test
```

这个基准只用于定位指标库热路径的 CPU 成本和验证输出形状；
off 模式只有共同的轻量原子工作，比例会大幅夸大实际业务开销。
`max_rss_kb` 是进程峰值而不是每条消息内存。不要在共享 CI 上
断言绝对 ns/op 或 off/basic 比例。
关闭时 Writer 热路径只做一次轻量 mode 原子读取并跳过计时和指标句柄复制；
既有 transmitter 快照/并发保护仍保留。Reader/队列路径不注册 endpoint，
空句柄快速退出。启用后的时间采样、原子统计与直方图同步有真实运行成本，
因此严格开销结论仍以稳定 runner 对照为准。

现有 `//tests/perf_test:cyber_rt_benchmark_suite` 使用原始 transport
transmitter/receiver，**不经过**目前埋点的 Node Writer/Reader，
因此不能用它对照 Runtime Metrics 成本。新增
`//tests/perf_test:runtime_metrics_pubsub_benchmark` 走真实 Node
Writer -> Reader 回调，以同一个进程内、独立执行的三种模式作对照：

```bash
bazel build //tests/perf_test:runtime_metrics_pubsub_benchmark
source scripts/env/runtime.bash
./bazel-bin/tests/perf_test/runtime_metrics_pubsub_benchmark off 100 1000 1024 1000
./bazel-bin/tests/perf_test/runtime_metrics_pubsub_benchmark basic 100 1000 1024 1000
./bazel-bin/tests/perf_test/runtime_metrics_pubsub_benchmark detailed 100 1000 1024 1000
bazel test //tests/perf_test:runtime_metrics_pubsub_benchmark_test
bazel test //tests/perf_test:runtime_metrics_compare_test
python3 tests/perf_test/runtime_metrics_compare.py \
  --binary ./bazel-bin/tests/perf_test/runtime_metrics_pubsub_benchmark \
  --rounds 3 --warmup 100 --messages 1000 --payload-bytes 1024 \
  --rate-hz 1000 --output ./metrics-comparison.json
```

参数为模式、预热消息数、测量消息数、payload 字节数和目标 Hz；
每次执行单独进程，避免序列/直方图状态跨模式污染。JSON 的
`received` 为排除预热后的回调数，`publish_count` 等是包括预热的
进程累计计数，用来确认模式**真实生效**（off 为 0）。
模式在 Init 后、创建 Node 前设置于指标注册表；此入口不测配置解析
或周期性文件导出成本。预热必须全部送达，否则本轮失败。`p99_ns`
是从写调用前的本地 steady clock 时间戳到回调的测量样本分位数，
不是跨进程传输延迟。`send_elapsed_ns` 是发送循环耗时，
`drain_elapsed_ns` 是其后等待交付的耗时；`elapsed_ns` 为两者之和，
`cpu_ns` 包含两阶段（包括限速 sleep），`max_rss_kb` 是整个进程的峰值。
基准 Reader 显式设置 1024 条待处理上限，避免默认单槽队列在短暂调度
抖动下丢弃；这不代表生产默认队列配置，积压超过该上限仍可能丢弃。
如果发生丢包，仅对已收到消息计算 p99，应同时查看 `received/messages`；
不同进程 PID/发现开销可能影响结果。`runtime_metrics_compare.py`
每轮轮换模式执行顺序，保留所有原始结果；只有每轮完整交付、参数匹配、
指标确实启用时，才输出各模式的发送耗时、CPU、p99 和峰值 RSS 中位数。
缺消息或执行失败会输出 `status: incomplete` 和原因、退出非零，
不会将已收消息的 p99 冒充完整交付结论。中位数只是观察值，
既不是置信区间，也不是性能验收；限速循环不能证明峰值吞吐能力。
在共享机器上不要以单次比率或这些中位数判断预算达标。

### 2026-09-30 Node 开启开销诊断基线

在共享开发机（x86_64、Intel i7-12700KF、20 逻辑 CPU、
31 GiB RAM）上，以未提交工作区（HEAD
`785baef4e0e7d46d0fb8e90693a32d07a420fea0`）执行
`bazel build --config=ci //tests/perf_test:runtime_metrics_pubsub_benchmark`，
`source scripts/env/runtime.bash` 后用上述 compare 脚本分别运行：

| 测量消息 / payload / 限速 | off/basic/detailed CPU 中位 (ms) | basic/detailed 相对 off CPU | off/basic/detailed 本地回调 p99 中位 (µs) | off/basic/detailed 峰值 RSS 中位 (MiB) |
| --- | --- | --- | --- | --- |
| 5000 / 256 B / 5000 Hz | 76.62 / 85.92 / 83.38 | +12.1% / +8.8% | 23.19 / 32.77 / 32.01 | 13.59 / 14.06 / 14.84 |
| 1000 / 1 KiB / 1000 Hz | 31.03 / 39.47 / 35.37 | +27.2% / +14.0% | 72.99 / 94.07 / 88.11 | 14.22 / 15.16 / 14.84 |

每种负载预热 100 条，各执行 **5 轮交错模式**（15 个独立进程）；
上述 30 次均完整交付、指标计数符合模式。发送时间中位数三种模式
分别约为 1000.052/1000.053/1000.053 ms（5000 Hz）和
1000.053/1000.061/1000.061 ms（1000 Hz），均由限速器决定，
不能据此宣称吞吐不受影响。5000 Hz 长试次各模式 CPU 的轮间范围为
69.63–90.15 / 77.65–91.12 / 78.77–99.22 ms；p99 范围为
22.18–30.60 / 29.31–84.38 / 26.27–38.91 µs。1000 Hz 的
off 回调 p99 甚至有 1174.74 µs 的单轮离群值。因此表中的
“相对 off”仅为**各模式中位数之比**，不是因果估计、置信区间或
生产环境最坏延迟；basic 比 detailed 高也不代表 detailed 更省资源。
两种启用模式目前具有相同埋点覆盖，不应从这组噪声推断模式梯度。
CPU 增量在 5000 Hz 长试次约为 1.86/1.35 µs 每条测量消息
（basic/detailed）；该数值包括两个 Node 的后台工作和限速期间的
调度，不是单条消息的纯埋点耗时。此处的 off 是**同版本关闭指标**，
没有与加入 Metrics 之前的源码版本做构建对照，不能用来量化
默认关闭时相对旧版的性能变化。

复现命令（每次仅改变负载参数与输出文件名）：

```bash
python3 tests/perf_test/runtime_metrics_compare.py \
  --binary ./bazel-bin/tests/perf_test/runtime_metrics_pubsub_benchmark \
  --rounds 5 --warmup 100 --messages 5000 --payload-bytes 256 \
  --rate-hz 5000 --output /path/to/runtime-metrics-compare-20260930-5khz-long.json
python3 tests/perf_test/runtime_metrics_compare.py \
  --binary ./bazel-bin/tests/perf_test/runtime_metrics_pubsub_benchmark \
  --rounds 5 --warmup 100 --messages 1000 --payload-bytes 1024 \
  --rate-hz 1000 --output /path/to/runtime-metrics-compare-20260930-1khz.json
```

另一次 2000 条、256 B、5000 Hz 的短试次同样完整交付，但
CPU 比值仅 +5.8%/+3.9%，说明采样窗口与宿主负载对估算影响很大。
5000 条、256 B、10000 Hz 的试次在第 4 轮 detailed 预热时
仅交付 99/100，脚本标记为 `incomplete`，**不能用于计算开销**。
四份原始 JSON（含不完整试次）位于本次会话
`files/runtime-metrics-compare-20260930-{5khz-long,1khz,5khz,10khz}.json`，
不入库。此基线只包含同进程 Node 收发及进程内指标；
不包含每秒快照/磁盘写入、跨进程传输、多消费者或固定 runner 验收，
不能作为默认开启 Metrics 的依据。

同日分别用 `runtime_metrics_hot_path_benchmark off|basic|detailed
10000 100000` 运行 5 次，得到中位 4.55 / 166.42 / 170.70 ns/次
（每次启用时记录一次发布、接收、队列和回调）；轮间范围分别为
3.84–5.27 / 165.43–179.15 / 166.06–175.47 ns/次，所有启用
试次的计数与预热加测量迭代数一致。这是**合成指标热路径**，off
几乎不做实际业务，不能用其约 37 倍的比例表示 Node 或应用损耗。
原始数据在会话 `files/runtime-metrics-hotpath-20260930.json`。

## 本次验证记录（共享开发机）

2026-09-27 执行 3 轮交错顺序对照：100 条预热、1000 条测量、256-byte
payload、限速 5000 Hz。六次执行全部完整交付；off 的 `live_series` 与
publish/receive/callback 计数均为 0，basic/detailed 各有 4 个序列且
累计计数均为 1100。off/basic/detailed 的中位 `send_elapsed_ns` 分别约为
200.052 ms、200.054 ms、200.054 ms；中位 `cpu_ns` 约为
28.43 ms、32.52 ms、32.75 ms，峰值 RSS 约为 23.0、23.0、23.2 MiB。

发送耗时被 5000 Hz 限速循环固定在约 200 ms，p99 与 CPU 在三轮间波动，
因此该结果只能证明 off/basic/detailed 的配置与输出契约有效，不能量化
峰值吞吐开销，也不能证明延迟改善或性能预算通过。原始 JSON 保存在本次
会话工作区，不作为仓库基准数据。

下一道**尚未完成**的门禁：在稳定 runner 上增加跨进程和多消费者
Node 链路，覆盖小消息高频、大载荷、fanout 和慢消费者，隔离发现与
预热，明确测量窗口和丢弃/吞吐口径后重复运行，按计划预算验收。
没有这个结果之前不建议默认开启。

## 周期快照的成本

`//tests/perf_test:runtime_metrics_snapshot_benchmark` 为指定数量的活跃
consumer 同时注入队列/回调直方图样本，单独测量进程内 `Snapshot()`
和 JSON 序列化的 p50/p99、CPU、JSON 字节数和峰值 RSS。
它不启动 exporter，也不测文件写入、Node 收发或最坏的退役历史叠加。
四个分位数现在只遍历一次每直方图 834 个桶；仍须验证峰值序列数下的
周期导出不会干扰业务线程，不用本地一次数值替代固定 runner 门禁。

```bash
bazel build //tests/perf_test:runtime_metrics_snapshot_benchmark
bazel test //tests/perf_test:runtime_metrics_snapshot_benchmark_test
./bazel-bin/tests/perf_test/runtime_metrics_snapshot_benchmark 4096 10
```

`series` 范围为 1..4096，`iterations` 为 1..1000；每次运行独立
进程，记录硬件、模式和业务负载后才可比较不同代码版本的开销。

2026-09-30 在共享开发机（x86_64、Intel i7-12700KF、20 逻辑 CPU、
约 32 GB RAM、未提交工作区）上分别启动独立进程，运行每进程 10 次
快照。下列为**诊断样本而非门禁**，p99 只有 10 个样本：

| 活跃 Consumer | Snapshot p50/p99 | JSON p50/p99 | 单次 JSON | 峰值 RSS (MiB) |
| --- | --- | --- | --- | --- |
| 256 | 0.45/0.46 ms | 0.95/1.20 ms | 0.67 MB | 9.6 |
| 1024 | 1.90/2.14 ms | 3.91/4.75 ms | 2.69 MB | 27.6 |
| 4096 | 8.94/9.22 ms | 16.36/18.82 ms | 10.76 MB | 100.5 |

4096 活跃端点若每秒写一次、且大小保持相近，文件写入量约
**38.7 GB/小时**；这只是 `json_bytes × 3600` 的容量预测，并没有
实际测量磁盘 I/O、闪存写放大、导出周期偏差或活跃端点再叠加
4096 条退役快照。导出是 opt-in，但在端侧不能因“有 4096 上限”
就将最坏资源开销视为可接受。固定 runner 须同时测同场景的
Snapshot+ToJson、真实周期文件写入与业务线程 p99，先约定
内存/CPU/写入预算和需要的刷新时效，再决定是否调整导出策略。

同日增加每规模 **30 次**独立进程内快照和序列化样本
（`runtime_metrics_snapshot_benchmark <series> 30`），结果如下；
这是上述 10 次样本的补充，不是文件导出的测量：

| 活跃 Consumer | Snapshot p50/p99 | JSON p50/p99 | 单次 JSON | 峰值 RSS (MiB) |
| --- | --- | --- | --- | --- |
| 256 | 0.17/0.63 ms | 0.83/3.91 ms | 0.67 MB | 10.5 |
| 1024 | 0.99/2.49 ms | 3.68/5.98 ms | 2.69 MB | 27.7 |
| 4096 | 4.67/5.20 ms | 15.60/38.19 ms | 10.76 MB | 100.4 |

4096 序列的 30 次执行累计消耗约 658 ms 进程 CPU（包括基准测量开销），
但不能据此直接计算真实每秒导出线程的 CPU 占比；需另测文件写入和
收发干扰。原始 JSON 在会话
`files/runtime-metrics-snapshot-20260930.json`，不入库。

## JSON 序列化优化与复测

优化前 `ProcessSnapshot::ToJson()` 用 `std::ostringstream` 逐字段
格式化、动态扩容，并为每个端点的四个标签分别构造转义字符串。
现改为预留容量的字符串写入器，整数和浮点数直接用 `to_chars`
写入，转义直接追加到输出。维持 schema v1 的字段、顺序、默认
六位有效数字、字符转义和导出频率；增加空快照及控制字符、
`uint64_t` 上界、浮点率的序列化断言。没有改动 pub/sub
逐消息指标路径或每秒一次导出策略。

在同一共享开发机上保留修改前的可执行文件，对每个规模交错运行
优化前/后各 **5 个独立进程**，每进程 30 次
`runtime_metrics_snapshot_benchmark <series> 30`。下表分别对
每个进程内 p50/p99 再取 5 次中位数；不是 150 次样本合并后的
全局分位数，单位 ms：

| 活跃 Consumer | JSON p50 前 → 后 | JSON p99 前 → 后 | JSON 字节数 | 峰值 RSS 前 → 后 |
| --- | --- | --- | --- | --- |
| 256 | 0.88 → 0.30（-65%） | 1.43 → 0.55 | 672,120 | 10.78 → 10.78 MiB |
| 1024 | 3.77 → 1.31（-65%） | 4.97 → 1.73 | 2,688,145 | 27.73 → 24.52 MiB |
| 4096 | 15.56 → 2.89（-81%） | 32.76 → 7.31 | 10,755,217 | 100.36 → 88.23 MiB |

4096 序列的 30 次快照加序列化进程 CPU 中位约从
689 ms 降为 184 ms；这不是单独导出线程的 CPU 比率。
共享机仍有抖动：256 序列优化后的单个进程 p99 达到
49 ms，4096 序列优化后的单个进程 p99 最高约 22 ms。
前后 JSON **长度一致不等于完整逐字节一致**，格式契约
另由 Metrics 单测与真实导出集成测试保护。原始 30 次进程结果
在会话 `files/runtime-metrics-json-ab-20260930.json`；
修改前的二进制在同一会话 `files/runtime-metrics-snapshot-before-optimization`。

此优化只减轻 CPU/分配成本：最大规模仍约 10.76 MB/份，
按当前每秒导出仍约 38.7 GB/小时，磁盘 I/O 和业务线程
干扰尚未实测。后续应在固定 runner 上分别测量**真实文件写入
与收发 p99**、不同活跃/退役端点组合及长时间写入成本，
先明确预算，再决定是否需要版本化紧凑格式或有界导出策略；
不能通过悄悄删 JSON 字段或改默认刷新频率来降低账面成本。

另用相同 5000 Hz / 256 B / 5000 条负载在优化后重跑 5 轮
Node off/basic/detailed 对照，15 次均完整交付；CPU 中位分别为
91.40/83.32/93.23 ms，本地回调 p99 中位分别为
28.18/32.89/31.99 µs。优化前分别为
76.62/85.92/83.38 ms 和 23.19/32.77/32.01 µs；
共享机绝对 CPU 波动明显，不能把两组直接相减声称收发改善或
退化。独立热路径重跑的 off/basic/detailed 中位为
3.89/179.98/172.65 ns/次，亦未显示稳定的逐消息收益。
本次改动没有改变 Node 基准测量窗口中的 `ToJson()` 调用次数
（为零）；JSON 性能收益属于单独的导出成本项。原始结果在会话
`files/runtime-metrics-node-post-json-20260930.json` 和
`files/runtime-metrics-hotpath-post-json-20260930.json`。
逐消息的 CPU 增量仍未解决；下一步应在固定 runner 上分别剖析
计时、直方图分桶、锁竞争和队列/回调更新，先确认主因及尾延迟预算，
再针对性优化，避免为追求合成 ns/op 改变采样与并发语义。
