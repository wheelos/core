# WheelOS Core 基础发布 Baseline

本项目的基础发布只关注三项可验证的核心能力：

1. Cyber RT 的基础通信、服务、调度、发现、记录和 Python API 可用。
2. 用户可运行的示例完整、路径一致，并能通过自动化 smoke/integration 测试。
3. 性能结果可复现、可归档，作为版本间比较依据。

## 架构边界

`cyber/` 只承载 Runtime 与公共 API：初始化、Node、调度、发现、传输、Record、
Mainboard 和 Python 绑定。`examples/` 是唯一用户示例入口；`tests/` 承载单元、
集成和性能验证。Runtime 目录不保留示例副本。

```text
cyber/       Runtime and public APIs
examples/    Runnable pub/sub, service, record, and component examples
tests/       Integration and performance validation
scripts/     Build, release, and report entrypoints
```

## 基础功能门禁

基础发布使用 Ubuntu 22.04、受版本锁定的 Bzlmod 依赖和已提交的
`MODULE.bazel.lock`。发布前依次执行：

```bash
bash scripts/release/check_bzlmod_lockfile.sh --check
bash scripts/release/ubuntu2204_baseline.sh --with-pycyber
bazel build --config=ci //examples/...
bazel test --config=ci //tests/integration_test:examples_regression_tests \
  --test_output=errors
```

`examples_regression_tests` 覆盖生命周期、Mainboard 错误处理、跨进程工具发现、
Record 回放、二进制 payload、fanout、fanin、payload 压力和 service burst。
核心 C++ 与 Python 单元测试由 `ubuntu2204_baseline.sh` 执行；新增 Runtime 功能
必须在对应 `cyber/` 包中增加确定性的单元测试。

每个示例必须满足：

- 有 Bazel 构建目标；
- 有最小运行说明；
- 不依赖 `/apollo` 等机器绝对路径；
- 至少由一个 smoke 或 integration 测试覆盖。

## 性能 Baseline

性能不与短时功能测试共用绝对阈值。基础发布归档结构化 JSON，并仅与同硬件、
同系统、同配置下的历史结果比较：

```bash
bash scripts/release/run_performance_baseline.sh
```

报告写入 `artifacts/performance/<UTC timestamp>/`：

- `baseline.json`：延迟、吞吐、丢失、重复、CPU、RSS、上下文切换；
- `summary.md`：按传输覆盖范围和消息类型汇总的可阅读结果；
- `benchmark.log`：执行日志；
- `metadata.txt`：Git SHA、Bazel 版本、内核与执行模式。

默认 `--quick` 用于 release candidate 冒烟。使用 `--full` 运行完整矩阵。若性能
结果中存在失败 case、核心 intra/SHM 1:1 场景出现非零丢失、异常资源增长，或相对
上一同环境基线明显退化，则阻断发布。跨主机模拟场景用于趋势观察，不应用作首次基础
版本的绝对性能门槛。

### 大消息 Protobuf 与 POD 比较

默认性能脚本额外执行同机跨进程 1:1 的大消息对比：SHM Protobuf 与 Iceoryx POD
以相同的 30 Hz 发送 1、4、7 MiB payload，每个点持续 3 秒。`summary.md` 的
`Large-message comparison` 表记录 p99 延迟、MiB/s、丢失率和 POD 零拷贝状态。
这用于确认同机数据平面的合理性；它不与 RTPS 跨主机路径混合比较。

Iceoryx POD 的单 chunk 容量约为 8 MiB，因此 7 MiB 是当前基础版本的最大对比点。
超过该上限应使用分片/分块协议或 RTPS，而不是将单个 payload 强行发送到 POD 通道。

#### 2026-08-05 同机大消息基线

以下结果来自 `artifacts/performance/protobuf-pod-20260805/`。所有 case 均为
独立 publisher/subscriber 进程、1:1、30 Hz、持续 3 秒；Protobuf 使用 SHM，
POD 使用 Iceoryx loan。两条路径均无发送失败和消息丢失。

| Payload | Protobuf p99 | POD p99 | Payload throughput | Protobuf CPU | POD CPU | Protobuf RSS | POD RSS |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 MiB | 1.129 ms | 0.468 ms | 30 MiB/s | 4.27% | 1.43% | 392 MiB | 96 MiB |
| 4 MiB | 6.034 ms | 1.192 ms | 120 MiB/s | 7.40% | 2.20% | 594 MiB | 196 MiB |
| 7 MiB | 9.241 ms | 3.473 ms | 210 MiB/s | 9.33% | 3.02% | 793 MiB | 298 MiB |

POD 在上述矩阵中每点完成 90 次借用发布且 `zero_copy_copy_count=0`。因此基础版本
的推荐策略是：同机大消息优先使用 POD/Iceoryx；需要 protobuf 语义或超过 8 MiB
单 chunk 上限时使用 Protobuf SHM，并采用分块传输控制内存占用。该表是同一机器上的
首个参考基线；版本间比较必须使用相同内核、CPU 绑定、Iceoryx 配置和脚本参数。

## 发布最小验收

发布候选必须具备：

1. 锁文件检查、基础发布脚本和示例构建全部通过；
2. 集成回归全部通过；
3. 性能 JSON、日志和元数据已归档；
4. `build_release_artifacts.sh` 生成的 native 包和 Python wheel 可安装、可启动。

不在基础版本强制要求完整覆盖率门槛、24 小时 soak 或跨主机性能比较；这些属于后续
增强门禁，不应拖慢首次稳定发布。
