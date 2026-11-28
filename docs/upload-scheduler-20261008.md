# 多节点波形上传调度（2026-10-08）

## 修改前的本地 Git 存档

四个工程均有标签 `backup/before-upload-scheduler-20261008`，仅保存于本地。

| 工程 | 路径 | 存档提交 |
| --- | --- | --- |
| 前后端 | `C:/Users/Administrator/Documents/Codex/Zigbee-Gateway` | `2698dd5` |
| ST 节点 | `C:/Users/Administrator/STM32-Projects-1.91/Zigbee` | `1928459` |
| ST 协调器 | `C:/Users/Administrator/STM32-Projects-1.91/Zigbee-coordinator` | `c05a0c3` |
| TI 协调器 | `C:/Users/Administrator/workspace_ccstheia/zc_cc1352R_coord` | `a74c4f3` |

TI 工程原先无 Git 仓库，此次初始化并存档项目本地源文件、工程配置。
已有 SDK 链接仍依赖 `C:/ti/simplelink_cc13xx_cc26xx_sdk_8_32_00_07`。
运行数据库、忽略的本机串口配置、依赖目录和编译产物不属于此代码存档。
需要查看旧代码时可用 `git show <标签>:<文件>`；恢复前应保留后续修改。

## 行为

- 节点仍可同时采集，采集结果先进入节点 Flash。
- 后端利用现有 `0x93` 块描述请求，在每个 512 点块开始前分配上传许可。
- 全网只有一个原始波形块持有许可。其他节点按 FIFO 排队；完成一个块后重新排队。
- 重复块申请保留当前许可；重复窗口完成 ACK 不会释放其他节点的许可。
- 数据分片仅接受当前许可持有者的当前块。失去许可后重新申请，不删除 Flash 数据。
- 正常 ACK 仍保持 3 秒等待、最多 5 次重试；收到明确 WAIT 不消耗失败次数。
- 节点起始分片间隔为 20 ms。APS 失败、缺片补传及 ACK 超时触发退避，最高 160 ms；成功完成块后逐渐恢复。
- 节点一次最多持有一个未完成的上传 APS 请求，避免查询消息越过尚未发送完成的数据。
- 队列等待为 500–750 ms。节点释放上传 fast-poll，待其他通信结束后允许 STOP2，由 RTC 定时器唤醒并重新申请。
- 节点许可为 20 秒；后端保留到 30 秒以覆盖下行、队列和时钟偏差。节点先停止再续领；同一块续领也有次数上限，持续故障会保留 Flash 并退出本轮。
- 后端启动后的首次调度请求起有 30 秒保护期，期间回复 WAIT，避免重启前的许可仍在使用时放行新节点。
- 10 秒未继续请求的等待者退出 FIFO；离线许可最多占用 30 秒。接收上下文沿用 16 个槽位，满时额外批次收到 WAIT。
- 旧批次优先、缺片位图重传、最终 CRC 和保存确认保持原语义；块完成仅是内存接收完成，只有全批次入库和特征确认后节点才释放 Flash。
- 前端显示“排队等待上传”，刷新页面通过现有接口恢复排队状态。

## 协议扩展

保持 V2 命令和 ACK 的 17 字节长度。ACK 偏移按不含串口短地址的载荷计：

| ACK | 状态 | 字节 9–12 | 字节 13–16 |
| --- | --- | --- | --- |
| BATCH / BLOCK | `V2_WAIT=7` | 建议等待毫秒数 | 保留 0 |
| BLOCK | `V2_OK=0` | 许可毫秒数 20000 | 最小分片间隔 20 ms |
| WINDOW | `V2_MISSING=1` | 原有 64 位缺片位图低半部分 | 位图高半部分 |

新节点仍能接收旧后端的零许可字段，但那种组合没有集中调度保证。
旧节点不理解 WAIT，可能重试耗尽；正式使用必须更新所有参与波形上传的节点及后端。
TI 协调器透传原有 ACK，新增预留 4 个 AF 发送槽位供 ACK 使用，失败通过 `9F/36` 输出提交/APS 确认状态。
本次 TI 文件只有 `Application/gateway_bridge.c`，它已是项目本地文件；没有修改 SDK 链接文件。TXPOWER 仍为 +5 dBm。

## 文件

- 网关：`gateway-core/include/vibration_scheduler.h`（新增），`vibration_gateway.h`、`vibration_wire.h`、`vibration_tx.h`、`gateway-core/src/main.c`。
- 前端：`gateway-ui/src/NodeDetails.jsx`。
- ST 节点：`Core/Inc/vibration_wire.h`、`Core/Inc/vibration_tx.h`、`STM32_WPAN/App/app_zigbee.c`。保留节点自己的查表 CRC 优化。
- TI 协调器：`Application/gateway_bridge.c`。
- ST 协调器此次仅存档，未修改源代码。
- 测试：`tests/test_upload_scheduler.c`、`tests/test_upload_multi.c`、`tests/run-upload-scheduler.ps1`；现有 `test_vibration_gateway.c` 使用统一模拟时钟。

## 验证与部署

运行 `tests/run-upload-scheduler.ps1`，测试使用内存 SQLite，不接触实际数据库或板卡。
覆盖 FIFO 公平、WAIT 不消耗重试、许可过期、消失的等待者、重复 ACK、RTC/HAL 时间衔接、退避上限，以及 4 节点和 17 节点同时传输中的丢片、丢 ACK、后端重启、持有许可节点掉线。17 节点用例还覆盖 16 个接收槽位满时的等待与恢复。实际波形接收仍执行 CRC 和数据库检查。
保留原有编解码、协议故障注入、数据库及串口适配器回归，并额外编译测试 ST 节点实际协议头。

编译检查：ST 节点全量编译链接，TI 协调器增量编译链接，前端生产构建，后端 Linux 目标对象编译。
硬件 RF、STOP2 实际电流、UART 压力、远近节点吞吐量尚需现场验证。

部署时更新后端及前端，重新构建并烧录两块 ST 节点和 TI 协调器；正常程序更新无需整片擦除。
ST 脚本产物位于 `Zigbee/PureZigbeeBuild/Zigbee.hex`，TI 位于 `zc_cc1352R_coord/default/zc_cc1352R_coord.hex`。
若使用 IDE 调试下载，请先 Build，避免下载旧的 Debug 产物。

现场依次验证远节点单独上传、两节点同时采集上传、上传中复位一个节点、上传中重启后端。
保存 `[上传调度]`、`TI振动ACK失败` 和节点 `V2 stats` 日志。观察两者均持续取得块许可、CRC 完成、旧批次最终确认后才被释放。
模拟测试耗时不代表无线吞吐率；当前策略优先保证公平和可恢复性，实际参数需依据现场日志调优。
