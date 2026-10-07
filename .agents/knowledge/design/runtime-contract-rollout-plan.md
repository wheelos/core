# Runtime Contract 兼容优先实施计划

状态：P0 部分实施（表征目标已加入必需 CI，固定 runner 待验收），
P1 已加入 dev/publish 分离和产物身份校验；开发版产物已在本机
Ubuntu 22.04 容器验证，正式发布 tag/确定提交验收仍待完成。
2026-09-29 审查记录见文末。
总体方向与长期阶段见
`../roadmaps/runtime-contract-evolution.md`；本文只规定近期变更的拆分、
不变式、验证门槛和回退边界。Runtime Metrics 与发布门禁已分拆提交；
先前混合工作区的性能样本仍只是诊断，不是确定提交的发布基线。
**2026 Q4/Core 1.1 仅交付 Correctness、Contract、Observability
基线；下文 P2/P3 新公开 API 和 P4/P5 新机制是后续候选工作包，
不属于 1.1 的功能范围。**Q4 可以修已证实的缺陷、增加表征测试
与文档，但不新增 Queue/QoS/Scheduler/Replay 策略或承诺旧插件 ABI。

已具备的 P0 证据：`//cyber/node:writer_test` 固定对象复制、共享指针身份与
提交结果；`//cyber/transport/integration_test:hybrid_transceiver_test`
固定无路由、部分成功及全部失败；`//cyber/data:channel_buffer_test`
固定首次 Fetch 与 overflow 取最新，`//cyber/node:writer_reader_test`
固定默认队列深度；`//tests/integration_test:runtime_metrics_test`
覆盖执行中 callback/Proc 的 shutdown；
`//tests/integration_test:cross_process_churn_test` 已覆盖多轮 Writer/Reader
双向重启、交付和单 Writer 序列递增，2026-09-28 非缓存重跑通过。
同日独立进程三轮 off/basic/detailed Node pub/sub 对照（100 预热、1000
测量、256-byte、5000 Hz）全部交付；原始 JSON 留在当前会话文件，
仅用于本开发机后续对照，**不是固定 runner 的性能门禁**。Metrics 改动
现已提交；固定 runner、多进程指标负载及完整故障矩阵仍欠缺；
`bash scripts/release/ubuntu2204_baseline.sh` 已于同日通过（25 个测试目标），
不能据此宣布 P0/P2 全部完成。

## 不变式

- 默认构建、配置和旧 API 的可观察行为不变：`Write()` 的 bool 返回值、
  无订阅者时 HYBRID 的返回、部分后端失败的返回、默认队列深度 1、
  首次 Fetch 取最新、慢消费者覆盖旧消息、callback 签名和 shutdown
  等待执行中 callback 的行为都要保持。先描述事实，不把现状包装成
  强送达或可靠传输保证。
- 不为仅用于观测的能力改变消息体、跨进程线协议、QoS、调度策略或
  SHM/GPU 资源回收规则；新元数据在兼容验证前仅进程内可用。
- 旧接口不增加虚函数、不再未经兼容验证改变已有对象布局、符号或公开
  回调类型。**当前实现已经给 `ComponentBase`、`Writer` 等类型新增
  成员，不能宣称这一不变式已满足。**新结果 API 先作为独立的 opt-in
  入口设计，并分别验证源码、链接。**本轮按要求不处理既有 ABI 边界，
  不声明旧插件与当前版本二进制兼容**；需要该保证的发布须另立门槛。
- 同一消息不能因为同时启用旧接口/新诊断而重复发送或记录重复成功；
  新诊断若无法观察到底层真实 backend 结果，必须返回
  `unknown`/`unsupported`，不能从 bool 推测 `no_route`、部分成功
  或 reader 已处理。
- 关闭模式不得增加逐消息分配、时钟读取或指标句柄拷贝；启用模式必须
  有有界内存、标签上限和独立性能预算。
- 新能力失败时应显式报告；不把失败包装成成功，也不在超时后假定
  callback 已停止或 GPU fence 已完成。

## 工作包与停止门槛

每个工作包独立变更、独立验收；未通过当前门槛不进入下一包。

| 包 | 交付与修改边界 | 退出门槛 |
| --- | --- | --- |
| P0：冻结现状 | 基于确定提交固定现有行为、版本基线及性能原始样本；为 Writer/Reader、HYBRID 和 shutdown 写契约文档与表征测试；声明当前不支持的语义 | 行为测试在确定提交上通过且被必需 CI 选中；旧插件 ABI 本轮不验收也不作兼容声明；记录未覆盖情况。固定 runner 是指标发布门槛，不用共享机结果阻塞纯语义文档 |
| P1：发布身份门禁 | 检查 `MODULE.bazel`、Debian `pkg_deb`、pycyber 派生版本、tag 与发布脚本测试；区分允许无 tag 的开发构建与必须有准确 tag 的正式发布 | 不一致阻止发布而不影响本地 dev build；所有 wheel/sdist（含 dev）核对内嵌版本；发布要求正确 tag；`PYCYBER_VERSION` 覆盖需单独规定；验证真实 deb/wheel、manifest 和安装 |
| P2：Writer 语义入口 | 文档化旧 bool 行为；先设计 backend capability 与可表达 `no_route`/partial 的提交结果，建立独立 opt-in 调用路径；不得先用 `bool Write()` 包一层虚假的结构化结果 | 新旧路径同一测试输入只提交一次；旧返回逐例不变；结果只报告能由 backend 证明的信息 |
| P3：Reader 上下文 | 先接进程内侧带元数据和可选新回调；保留旧回调、缓存、默认队列和调度；source time 不明时标为 unavailable | 两类 callback 对同一输入有一致 payload/顺序；队列溢出与 shutdown 结果等同基线；旧 binary/新 binary 互通前不发送新线协议字段 |
| P4：生命周期与故障基线 | 先做状态/ownership 图、teardown 顺序断言和 fault-injection；只有局部顺序无法安全处理的已复现问题才试点 Coordinator | Fast DDS 和全局清理顺序正确；阻塞 callback 不提前释放；重复停止及 crash/restart 无 UAF、误回收 |
| P5：兼容协议与性能 | 新旧节点双向互通后才启用版本化跨进程 metadata；先减少不必要的指标锁范围，仅在预算失败且收益已验证时引入 shard | N/N-1 正反向 wire、schema 和包验收通过；off/basic/detailed 在固定 runner 上满足事先校准的预算，未达到即不发布新模式 |

### P0：先获得可信的“零功能变化”证据

1. 固定一组最小行为矩阵，区分“调用被接受”和“消息送达”：
   - `Writer::Write(const M&)` / `Write(shared_ptr<M>)` / `Loan` / `Publish`；
   - HYBRID 无 reader、单 backend 成功、部分成功、全部失败；
   - INTRA/SHM/RTPS 的同步提交及接收端实际到达；
   - Reader 默认 depth=1、首次 Fetch、overflow 和 callback 持有 payload；
   - 阻塞 callback 期间 Shutdown、随后释放并验证队列退役；
   - 同一 writer 重连、不同 writer 并发，避免误断言全局有序。
2. 最近包内补单元测试：`cyber/node/`、`cyber/data/`、
   `cyber/transport/transmitter/`。跨进程与 shutdown 测试补在
   `tests/integration_test/`，沿用现有独占/本地标记和 bounded wait。
3. 在冻结的版本、配置、负载和相同 runner 上留存原始 off/basic/detailed
   样本；未全部交付的性能运行不得计算延迟或吞吐结论。性能样本只用作
   后续 diff 对照，不把共享开发机结果当门禁。

### P1：先挡住版本漂移，不改变运行时

先前 `MODULE.bazel` 为 `1.0.5`、根 `BUILD` 的 deb 为 `1.0.4`，
`scripts/release/release_scripts_test.py` 也使用 `1.0.4` 的包名；
现已对齐为 `1.0.5`，并新增
`scripts/release/check_release_version.py` 校验 module、Debian、release
tag、实际 deb 元数据及有 tag 发布时 wheel/sdist 文件名与内嵌版本。
先以 `MODULE.bazel` 为已使用的版本输入建立只读检查；不要在同一提交
同时增加另一份可人工编辑的版本源。检查应验证 release tag、deb、
wheel、manifest 的发布身份，且为非 tag 开发构建保留显式 dev 规则。
当前实现已将“dev 产物可构建”和“正式发布必须有匹配 tag”拆分：
`--publish` 要求 HEAD 的 tag 与 module/version 一致，CI 标记发布启用它；
所有产物（含 dev）检查包内 metadata 和文件名。
`PYCYBER_VERSION` 显式覆盖保留给无 tag 的开发包；带 tag 的版本必须
等于 module 版本，覆盖无法绕过正式发布校验。
之后再根据发布策略修正生产版本与测试夹具，不擅自把历史 release
产物改名或修改既有 tag。

### P2/P3：增量 API，不暗改语义

- 新 Writer 结果描述的是 backend 同步提交事实，不叫 delivery receipt；
  需有“无路由但与旧 bool 成功兼容”“部分 backend 失败”“无法细分错误”
  三类可表达状态。判断新旧接口等价时，比较同一调用的同步提交行为和
  旧 bool 投影，不把新 `no_route` 机械映射为 `false`。
- 先明确 `Transmit` / history / backend 的结果观测点与锁所有权，
  再选择新增独立方法或能力接口；避免把热路径改成两次发送或在
  `Write()` 外部再查询 topology 产生 TOCTOU 推测。
- 新 Reader 上下文只在数据已存在的时点赋值；`source_time`、
  `receive_time` 和 `steady_time` 标注 clock domain/availability，
  不以消息业务字段或本地时钟代填 source time。跨进程身份及 epoch
  留待 P5 协议协商，旧节点保持原有接收路径。

### P4/P5：内部替换须可回退

- 先用只读/审计事件与顺序断言验证 Reader/Task、Transport/Topology、
  GPU session 的现有依赖；只有局部修正不能处理的已复现故障才试点
  Lifecycle Coordinator。保留原清理入口作为版本内回退路径；
  不得同时迁移多种资源并引入新时序。
- deadline 到期必须返回未完成/隔离状态，并维持资源所有权；不能
  “超时即销毁”。SIGKILL/OOM 之后只能由新进程或外部 supervisor
  检测与恢复。
- wire metadata 要有 version、长度和 feature bits；先在双方协商
  可用时启用，旧节点只收到旧格式。保存 N-1 golden artifact、
  N/N-1 双向进程测试；不支持的后端只报告 unsupported。
- 先对既有 endpoint 锁和 queue 临界区做固定 runner 画像；若预算失败，
  优先尝试减少锁内工作。只有仍有经测量的 contention 才试点 metrics
  shard；shadow 对照现有 snapshot 的计数、drop 和 histogram 误差，
  限制双写负载与时长，不将 shadow 性能当生产模式预算。

## 每包统一验收与回退

1. **功能**：先运行最近 Bazel 单测，再跑受影响的集成用例；
   影响 RTPS/HYBRID 时运行
   `//tests/integration_test:examples_regression_tests`、
   `//cyber/transport/integration_test:rtps_transceiver_test`、
   `//cyber/transport/rtps:rtps_test`；影响整机生命周期或发布运行时
   时运行 `//tests/integration_test:core_tool_matrix_tests`。
   这些是不同变更的门槛，不要求每个文档提交跑全量。
2. **兼容**：记录 public API/ABI、wire/schema、配置默认值、包/工具
   输出是否变化；若任何项变化，必须有旧客户端/旧节点的对照或
   显式版本升级计划，不能仅以新版本单端测试代替。
3. **性能与资源**：热路径变更在相同 runner 对比 CPU、分配、
   lock contention、吞吐、p99/p999 和 RSS；先记录 baseline、
   波动及可接受预算，再判断回归。开启诊断的成本与关闭模式分别计算。
4. **失败即停**：功能/兼容矩阵红灯、无法证明的 delivery 语义、
   任一阶段出现 UAF/误回收或稳定性能回归时，停止推广并回退该包
   的新增入口/开关；已有 API 与资产保持原样。发现原有行为不安全时
   单独提出迁移和版本方案，不能悄悄以“修复”改写契约。

## 2026-09-29 架构审查与验证辩论记录

方法：架构审查提出代码/路线风险，验证方逐项质疑其证据边界，再用
反例与可证伪门槛交叉复核，共三轮。只记录当前代码可证实的事实；
测试通过不等于 ABI、跨版本 wire 或生产性能已被验收。本轮没有改动
Runtime/发布脚本的生产代码。

| 议题 | 第一轮：主张和证据 | 第二轮：反驳与边界 | 第三轮：决议和下一道门槛 |
| --- | --- | --- | --- |
| 插件与公开类 ABI | `cyber/component/component_base.h` 新增 `metric_`，`cyber/node/writer.h` 和 `cyber/node/reader.h` 也新增成员；`cyber/mainboard/module_controller.cc` 动态加载组件。这与上文“不改变旧对象布局”的承诺冲突 | 仅证明布局已变，不证明旧二进制必然崩溃；同版构建通过不能作为 N-1 插件证据 | **高优先级阻断 P0 冻结和 P2**：固定提交，用 N-1 编译的插件对新 mainboard 做加载/调用与 ABI 对照；若不支持混用，声明并版本化 ABI 边界，不能再宣称二进制兼容 |
| 表征测试与跨版本兼容 | `tests/integration_test/BUILD` 的 `core_tool_matrix_tests` 包含 cross-process churn/HYBRID/metrics，却不包含 `//cyber/node:writer_test` 和 `//cyber/data:channel_buffer_test`；churn 的两端来自同一代码版本 | 这表示已检查的必需基线有覆盖缺口，不表示没有任何 CI 运行这两个目标，也不证明现有 wire 有问题；N/N-1 wire 属于新增跨进程 metadata 的发布门槛 | **P0**：将上述单测与相关 HYBRID/metrics 目标纳入必需 CI 并在确定提交上记录新鲜结果；**P5 前**：用保留的 N-1 产物做新旧节点双向 wire 测试，未通过不得启用新 metadata |
| 版本身份与开发构建 | `scripts/release/check_release_version.py` 的 `--release` 允许 HEAD 无 tag；无 tag 时不检查 wheel/sdist 元数据；`build_and_package_pycyber.sh` 的 `PYCYBER_VERSION` 可覆盖自动生成的 dev 版本 | 无 tag 开发包允许构建，不等于 tag 触发的 PyPI 发布已被绕过；将 `--release` 一律要求有 tag 会破坏 dev 路径 | **P1 发布前**：区分 dev 校验与 publish 校验；publish 必须验证期望 ref/tag，所有生成的 wheel/sdist（含 dev）均校验内嵌版本与声明版本一致；明确覆盖参数许可条件，补无 tag/错误 metadata 负例 |
| 指标热路径 | `cyber/data/cache_buffer.h` 的队列操作在缓冲锁内执行端点/直方图更新；`cyber/metrics/metrics.cc` 用 mutex；限速 Node 基准不足以衡量最坏情况竞争 | 没有证明同一 mutex 重入死锁，也没有固定 runner 证据证明生产性能回归；微基准不是业务链路性能比率 | **Metrics v2 决策门槛**：先定预算并在固定 runner 的 fan-in/fanout 测锁争用、分配、CPU 和 p99/p999；若不达标先缩小锁内工作，仍不达标再试点 shard，并检验计数/分位数等价 |
| 生命周期架构 | `cyber/init.cc` 已有显式 Transport/Topology 清理顺序；路线图提出全局 Coordinator | 资源顺序复杂不等于需要立即引入通用依赖图；可先用顺序断言和故障注入验证现有清理 | **P4 决策门槛**：先跑重复关闭、阻塞回调、Fast DDS teardown、SHM/GPU 进程重启的有界故障测试；只有复现无法通过局部修正解决的所有权/顺序问题才引入 Coordinator |

本轮实际核查：`bazel query 'tests(//tests/integration_test:core_tool_matrix_tests)'`
只在上述目标中找到了 churn 与 metrics，没有 writer/channel buffer 单测；
以下三目标非缓存测试通过：

```bash
bazel test //cyber/node:writer_test //cyber/data:channel_buffer_test \
  //scripts/release:release_scripts_test \
  --nocache_test_results --test_output=errors
```

该结果只覆盖这些本地目标，
不推导为 N-1 ABI 或跨版本互通成功。之前的 Ubuntu 基线与开发机
性能样本保留其各自限定范围，不重复算作本轮新证据。

后续实施范围调整：**不处理 ABI 边界，也不声明旧插件兼容**。
接下来按顺序关闭：**P0 必需 CI 与确定提交证据 → dev/publish
版本门禁及完整产物安装 → 再评估 P2**。不要为了达成“架构先进”
提前引入 Coordinator、shard、delivery receipt 或新 wire 字段。

### 2026-09-29 后续实施复核（三轮）

1. **实施者**：Ubuntu 基线补选
   `//cyber/node:writer_test`、`//cyber/node:writer_reader_test`、
   `//cyber/data:channel_buffer_test`；HYBRID/metrics 已由原矩阵覆盖。
   **验证方质疑**：原矩阵只覆盖部分 P0；用 `bazel query` 核对
   新选择器闭包后才承认覆盖。确定提交和旧插件验证仍未完成。
2. **实施者**：仅 `--publish` 要求 HEAD 匹配
   `wheelos_core-v<version>`；CI 标记发布启用，上传前再次检查下载
   产物。无 tag 的默认构建仍生成 `.dev`；显式 `PYCYBER_VERSION`
   覆盖保留给非发布路径。
   **验证方质疑**：文件名正确不证明包内版本正确；因此 dev/tag
   的 wheel/sdist 都核对文件名和 metadata，测试无 tag、错 ref、
   错 override、包内版本漂移的负例。
3. **再次复核**：`//scripts/release:release_scripts_test` 非缓存通过；
   `bash scripts/release/ubuntu2204_baseline.sh` 包含新增目标，共
   28 个目标通过（其中 4 个执行、其余命中 Bazel 缓存）。
   当前无 tag 工作区的 `--publish` 被拒绝，`--python-version`
   输出 `.dev` 版本；真实 deb 元数据检查通过。
   **未验证**：带真实发布 tag 的完整 wheel/sdist 构建、auditwheel、
   干净环境安装与上传；不能把夹具测试当作发布验收。

### 2026-09-29 真实产物验证复核

1. **实施者**：Bzlmod lockfile 检查通过；在独立的会话输出目录调用
   `build_release_artifacts.sh --skip-baseline --outdir <独立目录>`，
   不覆盖 `artifacts/release/` 中的历史产物。原计划验证真实
   native deb、pycyber wheel/sdist、auditwheel 与安装。
   **验证方质疑**：构建在 pycyber 扩展 Bazel 编译阶段报
   `Server terminated abruptly (error code: 14, 'Socket closed')`；
   输出目录不包含本轮验收完毕的产物，不能沿用先前的 deb 或夹具
   测试宣称整个发布路径成功。
2. **实施者再试**：以 `--jobs=2 --local_ram_resources=4096` 单独
   预构建 `//cyber/python/internal:_cyber_wrapper.so` 和
   `//cyber/proto:record_py_pb2`，仍因 Bazel server 断开退出 37。
   当时共享机器内存约 31 GiB、swap 15 GiB 近满，其他进程占用
   较多内存；`jvm.out` 仅有 JVM 启动警告，未取得内核 OOM 证据。
   **再次复核**：资源压力是可能原因，不是已证实的根因；日志中也
   没有足以定位为源码编译错误的诊断。停止在共享机器上重复重编译。
   开发版本解析为 `1.0.5.dev144+g785baef`，无匹配 tag 的
   `--publish` 仍拒绝发布，`git diff --check` 通过；这些检查
   不替代真实产物验收。

**当前结论**：P1 的发布脚本与单测门禁已建立，但本轮 native/pycyber
完整构建、auditwheel、wheel/sdist 安装和工具运行仍被 Bazel
server 异常阻断。须在资源稳定的独立 runner 上从确定提交重跑
完整发布入口（不使用 `--skip-baseline`）、核对 manifest/校验和及
包内版本，并分别安装、运行验证后才能关闭 P1。此结论不包含
旧插件 ABI 兼容声明。

### 2026-09-29 资源受限重试与产物反例

1. **构建复核**：单任务、受限 Bazel batch 构建 pycyber 扩展和
   `//:wheelos_core` 成功。首次完整聚合构建完成 deb、wheel/sdist
   及 auditwheel repair，却被版本校验器误拒：真实 sdist 同时含根目录
   `PKG-INFO` 和嵌套 `egg-info/PKG-INFO`。**验证方质疑**：只检查
   “存在某个正确版本”的嵌套文件可掩盖根目录版本漂移；校验器改为
   精确读取 `pycyber-<version>/PKG-INFO`，测试固定“根目录错误、
   嵌套正确必须拒绝”和“根目录正确、嵌套不同仍以根目录为准”。
2. **再验证**：独立目录的开发版完整聚合（`--skip-baseline`）成功：
   `wheelos_core_1.0.5_amd64.deb`、`pycyber-1.0.5.dev144+g785baef`
   的 sdist 和 auditwheel 修复的 `cp310` wheel；修复标签为
   `manylinux_2_35_x86_64`，**不是**更老 glibc 的 manylinux
   保证。包内版本、twine、两级 SHA256SUMS 和 manifest 均通过，
   native bundle 工具入口、wheel 安装及 Python 示例 smoke 通过；
   最终 sdist 在全新 Python 3.10 venv 安装导入通过。
3. **独立环境质疑**：宿主机成功不能证明 Ubuntu 22.04 非 root
   客户安装。将最终产物只读挂载到 `wheelos-core-validation:x86_64`
   容器，以非 root `wheelos` 用户在两个全新 Python 3.10 venv
   分别安装 wheel/sdist，导入 `pycyber`、`cyber`、`record`、
   `record_pb2`，并解压 deb、source `setup.bash`、检查四个工具入口
   及运行 `cyber_launch --help`，均成功。这**不是**完整容器
   runtime/example/record-play 故障矩阵；宿主机上的基线为
   28/28 目标通过，其中 2 个本轮执行，其余使用缓存。
4. **发布身份再质疑**：当前工作区未提交改动，开发版 manifest
   的 `git_sha` 仅指向 HEAD，不能证明产物等于该提交；因此
   `--publish` 新增 clean-worktree 门禁（含未跟踪文件），负例单测
   通过；CI 上传作业把下载产物放到 runner 临时目录，避免
   仓库内 `dist/` 被误判为源码改动。正式 tag 的真实产物构建及
   从确定提交的独立 runner 验收仍未执行，不能把 `.dev` 安装
   测试宣称为正式发布通过。

默认并行的 Ubuntu 基线首次在约 3,366/3,399 构建动作时再次遇到
Bazel server `Socket closed`；单任务预构建相同目标后重新运行
**原基线入口**通过。资源敏感性尚未定位为 OOM 或源码错误；
后续在独立 runner 应复现并监控资源，不以预热缓存后的成功掩盖
冷构建风险。`//scripts/release:release_scripts_test` 的新增反例已
非缓存重跑通过；当前输出只证明开发版同版本安装，不证明 N−1
wire 或旧插件 ABI 兼容。

### 2026-09-29 晚间增量验证与门槛复核

1. **交付环境**：锁文件检查仍通过。以最终 deb 的 `setup.bash`
   配置非 root Ubuntu 22.04 容器后，`mainboard --help`、
   `cyber_recorder record --help`、`cyber_monitor -h`、
   `cyber_launch --help` 均返回成功；顶层
   `cyber_recorder --help` 实际打印用法但返回 255，是现有 CLI
   语义，不可将其记作成功退出。交付 wheel 的时间、timer、参数
   服务及 record 读写示例在同一容器通过；record 读取报告
   io_uring 不可用而回退同步路径，该结果不覆盖 io_uring 性能。
   未 source 包环境、工作目录不可写时 Python timer 曾因缺少
   `/apollo/cyber/conf/cyber.pb.conf` 失败，按交付要求配置后通过。
2. **非缓存运行矩阵**：宿主机以受限 Bazel 并发运行
   `//tests/integration_test:core_tool_matrix_tests`，24 项中
   23 项通过；`cross_process_churn_test` 在第 4 轮组件重启
   等待交付超时，单独非缓存重跑通过。记录为一次矩阵失败加一次
   定点通过，**不是** 24/24 首次通过，也未证明抖动根因。
3. **容器源码矩阵**：复制当前脏工作区的源码（排除 host Bazel
   缓存、vendor 和历史产物）进入非 root 容器，从头构建约
   3,634 个 Bazel 动作。24 项中 22 项通过，包括 churn、
   Python 示例和 tools；`transport_test` 与
   `core_transport_matrix_test` 在默认 64 MB `/dev/shm` 下为
   iceoryx 约 284 MB 段触发 SIGBUS/RouDi 超时。提高容器
   `/dev/shm` 到 1 GB 后，两次定点重试均被 Docker
   `SIGTERM` (15) 中断在测试前；没有证据将这两项改判通过，
   也没有证据认定中断为 OOM。后续需在不被中止、至少 1 GB
   `/dev/shm` 的独立 runner 仅重跑失败目标，再完成整套矩阵。
4. **泄漏证据分级**：仓库 Valgrind 矩阵的 5 项全部退出 0，
   definite/indirect loss 均为 0。record 示例与本地生成
   非空 record 的 bounded reader 也满足现行脚本的
   definite/indirect 门槛；但分别有 17,925 与 1,750 字节
   `possibly lost`，后者归因栈包括 Protobuf 静态 descriptor/
   Abseil 分配。现行脚本不把 possible 纳入失败条件；这些栈
   不足以证明持续增长的 Runtime 泄漏，却也**不满足**
   更严格的 possible=0 稳定性门槛。未运行固定 runner 的
   性能对照或两小时 pub/sub，P5/完整发布资格仍未验收。

交付顺序为：P0 与 P1（相互独立）-> P2 -> P3 -> P4 -> P5。
每一步的验收记录需包含代码提交、测试目标、环境、结果和剩余
unsupported 项。未完成某包的门槛时，不能宣称整个 Runtime Contract
已经完成。

### 2026-09-29 验证故障修复与反证

1. **记录读取路径**：保留 io_uring 初始化失败时的可移植同步回退，
   但日志现在输出实际 errno；显式 `uring_stream` 性能模式的初始化
   和缓冲区注册失败则直接报错，不回退。宿主机本地非空 record
   的显式模式读取了 1 个 chunk、100 条消息；此前容器中的 Python
   record 内容测试实际使用同步回退，**不能**据此宣称容器 io_uring
   性能已验证。
2. **跨进程抖动反证**：长时间运行的 recorder player 每约 100 ms
   向未读取的 stdout pipe 打印进度，可能反压播放线程。仅把不消费
   stdout 的 recorder/player/component 子进程改用 `DEVNULL`，
   保留需要断言输出的 pipe；churn 单独三次非缓存运行均通过。
   最新宿主机完整 `core_tool_matrix_tests` 非缓存运行 **24/24**
   通过，包括 churn，但不将此结果外推到容器。
3. **RouDi 锁权限**：宿主机遗留的跨用户 0644 `/tmp` 锁文件使
   `O_RDWR` 打开失败；改用 `O_RDONLY|O_CREAT` 仍受
   `fs.protected_regular=2` 限制而失败。最终先以只读、`O_NOFOLLOW`
   打开已存在文件，仅缺失时用 `O_EXCL` 创建并设置 0644；
   不 unlink 锁路径，以免后续进程锁到不同 inode。新增集成测试
   用预存的只读锁夹具，单目标和完整宿主机矩阵非缓存通过。
   这不证明跨用户共享 iceoryx 数据面可用；不可读的 0600 锁仍
   明确报错。
4. **资源与容器反证**：Core 开发 Compose 默认 `/dev/shm` 调整为
   可覆盖的 1 GB；apollo-lite 的开发容器参考配置默认 2 GB。
   iceoryx 单段约 284 MB 不等于并发矩阵只需 512 MB：512 MB
   诊断容器及此前 1 GB 重试都被 Docker `SIGTERM` 中止，
   不能判定容量是否充足或断言 OOM。需在稳定的非 root、
   私有 IPC、具备 memlock/seccomp 权限且至少 1 GB `/dev/shm`
   的独立 runner，从**本轮重建**的产物完成整套容器矩阵和
   io_uring 专项；此前安装通过的 deb/wheel 早于本次修复。
5. **泄漏门槛质疑**：Valgrind 脚本现在把 `possibly lost` 计入
   失败，脚本负例测试通过。实际 bounded reader 的日志显示
   definite/indirect 均为 0、possible 为 1,750 字节（18 blocks），
   3 个归因上下文可见 Abseil/Protobuf generated descriptor
   初始化；进程按预期非零退出。静态初始化栈不能证明无泄漏，
   暂不做泛化 suppression，也**不**标记严格门槛通过。后续在
   独立 runner 复核归因与增长性，再决定是否仅针对可证实的
   第三方常驻分配做精确豁免。

本轮仍未完成确定提交的发布验收、固定 runner 性能预算、两小时
pub/sub 稳定性和严格 Valgrind 门槛；旧插件 ABI 不在本轮范围内。

### 2026-09-29 Docker 中断复核

Docker 历史事件记录了两次容器收到 `kill signal=15`、以 143 退出；
第二次在启动约 6 秒后发生。没有内核 OOM 记录，也没有可确定
SIGTERM 发起者的审计记录；不能把这两次归咎于 Core、512 MB
或 1 GB `/dev/shm`。原 `docker run --rm` 的附着模式会转发
客户端信号，并立即删除容器，妨碍事后归因。交付验证指南改为
`docker create` / `start` / `wait`，保留 ID、日志及退出状态；
等待端中断后仍可检查、继续等待，不会自动清理容器。

以当前宿主机编译的二进制（**不是**容器冷构建）在 Ubuntu 22.04
非 root、私有 IPC、1 GB `/dev/shm`、unconfined seccomp 与
unlimited memlock 容器内复核：`transport_test` 的 6 个
iceoryx 用例、`core_transport_matrix_test` 的 2 个用例通过；
显式 `uring_stream` 读取本地 record 成功（1 chunk、100 条消息）。
独立 `create` / `start` / `wait` 定点运行也通过，容器
`Exit=0`、`OOMKilled=false`。本轮按用户要求**跳过 Valgrind**，
泄漏门槛记为“未验收”，不再因该门槛阻断本次容器故障排查；
冷构建全矩阵、长期稳定性和发布验收仍未完成。

## 下一步执行计划（2026-09-29 更新）

按以下顺序推进；每一步保留源码状态、命令、容器镜像 ID、退出状态、
日志和结果文件。前一步失败时先定位，不以宿主机通过替代容器验收。

1. **P0：关闭容器验证缺口。** 使用文档中的
   `docker create` / `start` / `wait`、非 root、私有 IPC、
   1 GB `/dev/shm`、unlimited memlock 和 unconfined seccomp。
   在独立可写的源码快照中排除宿主机 Bazel 输出、缓存与历史产物，
   从该快照在容器内构建并
   **非缓存**运行 `//tests/integration_test:core_tool_matrix_tests`；
   分别记录 24 项结果以及 `transport_test`、
   `core_transport_matrix_test`。要求 24/24、容器退出码 0、
   `OOMKilled=false`。如再次退出 143，保留容器及 Docker
   事件、daemon/调用端日志，先查明谁发送 SIGTERM，再重试；
   不按 OOM 或 512 MB 不足处理。512 MB 仅在 1 GB 基线通过后
   另做隔离容量实验，不作为当前矩阵默认值。
2. **P1：验收同源产物。** 从上述源码重建 native deb、pycyber
   wheel/sdist，在全新非 root 容器安装；核对版本、manifest、
   SHA256 和包内元数据，执行工具及 Python smoke、record
   读写，并以显式 `uring_stream` 模式确认 io_uring 路径。
   同步回退的内容正确性与 io_uring 的可用性分开报告；
   脏工作区开发包只能标记为开发验证，正式发布仍需干净确定
   提交和匹配 tag。旧插件 ABI 不在此轮范围。
3. **Q4 Contract：固定既有语义，不增加公开 API。** 把
   `Write()` 的接受/提交而非送达、无路由/部分后端失败、
   Reader 的时间来源、默认 depth=1、溢出与 callback/shutdown、
   以及资源 owner、teardown 顺序写成支持/未知/不支持矩阵；
   每项关联邻近单测或跨进程反例。公开的结构化结果与
   Reader context 仅设计其能力边界，留到 1.1 后以 P2/P3
   独立验收；不修改线协议或默认行为。
4. **Q4 Correctness/Observability：故障与开销门槛。**
   对现有清理顺序、重启和资源所有权做有界故障注入；
   仅修复可复现的缺陷，不预先引入 Coordinator。固定 runner
   上分别测 off/basic/detailed、真实 Node 链路和关闭路径，
   先校准并冻结资源/尾延迟预算，再做接受或回退决定；
   两小时 pub/sub 单列吞吐、包完整性和损失结果。
   开发机 smoke、容器 io_uring 单次读取都不等于性能或
   长时稳定性通过；后续 P4/P5 新机制不纳入 1.1。

Q4 排期以退出门槛而不是日历自动推进：

| 阶段 | 交付边界 | 停止条件 |
| --- | --- | --- |
| 10 月：Correctness | 确定源码快照、容器冷构建、24/24 运行矩阵；收敛已复现的 churn、iceoryx、record 及清理故障 | 有失败即保留日志与容器状态并定位，不以宿主机成功替代；版本和产物只对同一源码作结论 |
| 11 月：Contract | Writer/Reader/生命周期事实表、支持范围和反例测试；同源交付包版本与安装校验 | 每项 MUST 对应测试或明确 unsupported；不从 bool 推断送达，不声明未验收的 ABI 或 N−1 兼容 |
| 12 月：Observability | opt-in Metrics、关闭模式及固定 runner 对照，资源/标签边界与两小时运行证据 | 预先确定预算，缺消息或结果不全即未通过；未做的泄漏验证单列“未验收”，不得宣传完整稳定性 |

**本轮明确豁免 Valgrind**：允许先完成以上容器故障排查、
契约实施与其他独立验证；泄漏项持续标记“未验收”，不报告
稳定性或正式发布资格为全部通过。若以后恢复该门槛，应先
对已记录的 `possibly lost` 做可复现、精确的归因，不添加
宽泛 suppression 来制造通过结果。

### 2026-09-29 Q4 Correctness 冷容器复核

从当前未提交工作区制作独立可写源码快照，排除宿主 `bazel-*`、
vendor、历史 artifacts 和本地 `.bazelrc.user`；保留 Git HEAD
`785baef4e0e7d46d0fb8e90693a32d07a420fea0`、修改内容
与 untracked 文件指纹。镜像 ID
`sha256:4a5e18e0132250192fc8010fab43ffb3c075b539081281dd300f249854bbea44`，
Ubuntu 22.04、Bazel 7.6.2，非 root、私有 IPC、1 GB `/dev/shm`、
unconfined seccomp、unlimited memlock；不挂宿主 Bazel 缓存。
lockfile 检查通过，限制 2 个 Bazel jobs 和 3072 MB 构建资源后，
`bazel --batch test --config=ci --nocache_test_results
//tests/integration_test:core_tool_matrix_tests` 从源码执行
3,634 个动作，**24/24 通过**，容器 `Exit=0`、
`OOMKilled=false`。原始日志保存在当前会话
`files/core-11-cold-matrix.log`，快照在
`files/core-11-cold-20260929/`。

这关闭了先前 64 MB `/dev/shm` 容器矩阵的本轮复现缺口，
但不是正式 tag 的确定提交，也不等于完整 Ubuntu 发布基线、
交付产物安装、固定 runner 性能或两小时稳定性。Valgrind
按本轮豁免，持续标为未验收。

### 2026-09-29 Q4 发布卫生与同源产物复核

沿用上述冷容器的源码快照及 Bazel 编译缓存，以 2 jobs / 3072 MB
运行**原** `ubuntu2204_baseline.sh`：28/28 目标通过（4 个本轮
执行，其余复用该快照先前测试缓存）。首次完整产物构建在
auditwheel repair 报错：Ubuntu 22.04 系统 `patchelf 0.14.3`
低于 auditwheel 的 `>= 0.14.5` 要求；没有跳过 repair。
pycyber 打包脚本现于构建 venv 安装满足要求的 patchelf，
仅为 auditwheel 选择该 venv 的可执行文件。同步这一脚本修复
后重跑聚合入口的产物阶段（前述 Ubuntu 基线独立已通过），
native deb、repaired wheel 和 sdist 均构建成功；wheel
标签为 `manylinux_2_35_x86_64`，并非旧 glibc 的兼容承诺。

两级 SHA256SUMS、manifest、deb `1.0.5` 元数据均核对通过；
新的非 root Ubuntu 22.04 容器只读挂载这些产物后，
deb 的四个工具帮助入口成功，wheel 与 sdist 分别在全新
Python 3.10 venv 安装导入成功。聚合打包脚本中的全新
venv 示例覆盖了 Python record 写入/读取；从该快照
另行编译的 `record_perf_reader` 在非 root 容器显式
`uring_stream` 读取 1 chunk / 100 条消息成功，没有同步
回退。这仅验证路径可用，不是吞吐基准。

快照仍含未提交改动；manifest 的 `git_sha` 只标识 HEAD，
不能证明这些开发产物等于该提交。正式 tag/干净提交的
发布资格、旧插件 ABI、固定 runner 性能、两小时稳定性
仍未验收；Valgrind 本轮豁免。下一个工作包转入 Q4
Writer/Reader/生命周期事实表和反例测试，不新增公开 API。

### 2026-09-30 Q4 Contract 行为基线

`docs/guides/runtime-contract-v1.md` 汇总已验证的 Writer 提交
而非送达、Reader 本地时间/队列/回调，以及 graceful 清理
边界，并逐项指向源码和最近包测试。检查 HYBRID 时发现
“没有 active reader 时 `true` 等于历史保存”的误读：
`History::Add` 在默认 volatile 模式直接返回；给既有
`WriteResultIsAnyActiveBackendAccepted` 增加无 reader 时
history 仍为空的断言，并修正路线图措辞。HYBRID 定点
非缓存测试 1/1，Writer/Reader/ChannelBuffer、Runtime
Metrics shutdown 和 framework lifecycle 定点非缓存 5/5
通过；没有改变业务运行路径、公开 API 或默认 QoS。

目前不能推出跨 Writer 全序、source timestamp、Callback
deadline、SIGKILL 后安全回收、GPU fence 完成或 N−1/旧插件
兼容性。这些仍是明确的 unknown/unsupported 范围，下一步
仅围绕已复现的资源所有权/清理反例增加测试，不预先引入
通用 Coordinator。
