# 9D2C 节点配置查询发送诊断

目标工程为 `C:/Users/Administrator/STM32-Projects-1.91/STM32WB55RGV6-BLE-Zigbee`，不是纯 Zigbee 工程。只给 9D2C 烧录这版 CPU1 应用，协调器和其他节点保持原样。

## 构建与烧录

Debug 和 Release 均已构建。可使用 STM32CubeIDE 对该工程执行正常应用下载，或用支持 ELF 的下载工具加载：

```text
C:/Users/Administrator/STM32-Projects-1.91/STM32WB55RGV6-BLE-Zigbee/Debug/STM32WB55RGV6-BLE-Zigbee.elf
```

不要为此次诊断做整片擦除、删除网络或重烧 CPU2 无线协议栈。正常应用下载会重启节点，重启本身会改变异常现场；本次需要重新观察故障是否再次出现。

启动串口应出现：

```text
ZIGBEE: build ble-zigbee-flash-arbitration-20261026 | <编译时间>
ZIGBEE-CFG: enabled=1; trace=enqueue/task/submit only; no extra APS callback
```

节点诊断在源文件顶部由 `ZB_CONFIG_TX_DIAG_ENABLED` 控制，默认 1。设为 0 后重新构建即可关闭详细诊断，启动时仍显示开关值。没有增加定时器、重试、网络请求、APS 回调或修改空口载荷；低功耗、配置查询重试预算、CPU2 固件和 BLE 业务保持原逻辑。串口打印有少量耗时，故障未复现不能单独作为问题已修复的证明。

## 标记

| 标记 | 含义 |
| --- | --- |
| `REQUEST-DEFER` | 网络恢复或公告尚未结束，原有流程暂缓配置查询 |
| `REQUEST-NO-EP` | 尚未学习协调器私有端点，原有流程安排稍后再试 |
| `REQUEST-OFFLINE` | 节点本地连接条件未满足，没有提交查询 |
| `REJECT-DEPENDENCY` / `REJECT-STATE` / `REJECT-FULL` | 发送函数拒绝入队，分别检查端点/Cluster、状态/参数、队列容量 |
| `ENQUEUE` | `0x99` 已进入节点本地发送队列，不证明已发送 |
| `REQUEST-RESULT` / `RETRY-RESULT` | 首次/重试调用发送函数的返回值，不是无线确认 |
| `BLOCK-PERSIST` | 发送任务被 `persist_tx_hold` 阻塞 |
| `BLOCK-RADIO` | 发送任务被 `radio_exclusive` 阻塞 |
| `BLOCK-INFLIGHT` | 发送任务受原有在途请求上限限制 |
| `DROP-TX-STATE` | 出队前发现温度 Cluster 或入网状态异常，原有逻辑清理队列 |
| `SUBMIT-BEGIN` | 即将调用 `ZbZclClusterCommandReq` 提交这条查询 |
| `SUBMIT-END` | 提交函数已经返回；`rc=00` 只表示协议栈 API 接受，不是 APS/MAC 投递确认 |
| `QUERY-TIMER` / `QUERY-EXHAUSTED` | 查询定时器触发/原有重试预算用完 |

只有队列中存在配置查询时才记录发送任务阻塞，重复相同阻塞最多每两秒记录一次，不主动重新调度任务。

## 字段

每行已有节点启动后的 RTC 时间前缀，之后显示：

- `r`：查询原因（十六进制）；01 入网/恢复，02 上传后，03 待机。
- `n`：打印时的配置查询尝试计数，**不是唯一报文编号**。队列可能包含更早重试的查询，不能强行一一配对。
- `rc`：该标记对应函数的返回值；没有调用结果的阶段使用 ff 占位。必须结合标记解释，不能把所有 ff 当作发送失败。
- `ep`：协调器目标端点，正常应已学习到非零值。
- `cl`：控制、数据、温度三个本地 Cluster 指针是否存在，例如 111；这不是无线接收确认。
- `q`：本地普通发送队列条数；`fly`：原有、需要回调的在途命令计数。0x99 仍不新增回调、不占用该计数。
- `p`：本地缓存的父节点在线标志，不证明当前通信成功；`join`：本地入网状态码。
- `hold`：持久化暂停应用发送的标志；`ex`：无线独占标志。
- `pause=a/b`：MAC 已暂停/暂停请求尚在处理的缓存标志。
- `rec`、`ann`：网络恢复/公告流程是否仍在进行。
- `fp`：配置事务长期快速轮询句柄是否存在，不证明 CPU2 正在实际轮询。入网时另有临时轮询请求，不由这个字段完整表示。

## 与后端联合复测

后端保持 `ZIGBEE_CONFIG_DIAG=1 ./gateway-server`。只重新烧录 9D2C，不增加节点或改变监测设置；观察从恢复网络、收到配置到后续查询失效的全过程。故障可能在首次十分钟待机窗口内出现，不要只保存睡醒后的片段。

- 没有 `ENQUEUE`：看 REQUEST/REJECT 标记，确认发送前提或入队失败。
- 有 `ENQUEUE`，没有 `SUBMIT-BEGIN`：看 BLOCK 标记；若也没有 BLOCK，检查发送任务是否得到执行。
- 有 `SUBMIT-BEGIN`，没有对应的 `SUBMIT-END`：同步协议栈调用可能没有返回，需继续结合后续串口任务判断；并发请求不能仅凭相邻行精确配对。
- `SUBMIT-END rc!=00`：协议栈 API 提交失败。
- `SUBMIT-END rc=00`，后端没有 9D2C 的 `RX99`：尚未证明空口投递，需要继续检查节点无线发送、协调器接收/过滤/转发，不能归因于配置回复缓存。
- 后端有 `RX99`：再看 `TX0C`、`SUBMIT8E`、`CONFIRM90`。

此版本没有增加 APS 完成确认观测；不把 API 返回成功伪装成 APS ACK。日志不包含网络密钥或传感器原始载荷。

## 回归测试

```powershell
powershell -NoProfile -File tests/run-node-config-diag-tests.ps1
powershell -NoProfile -File tests/run-node-recovery-tests.ps1
```

测试提取实际节点函数，在诊断 0 和 1 两种情况下检查查询载荷、回调选择、入队/拒绝、三类阻塞、API 提交错误、重试预算及待机定时策略均不改变，并检查诊断行不超过节点 UART 日志缓冲区。
