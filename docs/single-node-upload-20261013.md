# 恢复单节点分块上传（2026-10-13）

仅修改当前 `STM32WB55RGV6-BLE-Zigbee` 节点和网关后端的上传路径。
以节点历史提交 `1928459`、网关历史提交 `2698dd5` 的单节点 V2 分块流程为基础，保留现在的 Flash、特征值确认、诊断计数和有界补传保护。

## 当前流程

1. 采集结果保存到 Flash，节点发送批次描述，后端普通 ACK 确认。
2. 节点读取一个 512 点分块、压缩，发送 16 字节分块描述；后端发送 17 字节普通分块 ACK。
3. 收到许可后连续发送该块的分片，不等待每帧的后端回复。提交间隔恢复为 12 毫秒；实际间隔还受 APS 完成回调、缓冲区可用性和任务运行时间影响，不能据此保证实测吞吐。
4. 分片发完，等待 100 毫秒后发送块末查询。后端返回缺片位图或成功确认。缺片只补缺的分片，再查询；成功后处理下一块。
5. 全部块完成，发送 FINAL 查询。后端整批校验并入库后确认；节点再发送特征值摘要。原始波形和摘要都确认后才释放 Flash 记录。

ACK 等待超时仍为 3 秒，最多重试 5 次。补传和块末超时另有有界计数；失败保留已提交的 Flash 批次。
后端接收表满时仍可回复 WAIT，防止覆盖未完成数据；这不是空口排队或分组调度。

已移除上传时隙、组轮询、时钟校准、100 毫秒 RTT 拒绝、租期续约、无时隙丢弃分片，以及上传器额外的 NLME-SYNC 轮询。休眠节点下行回复仍由协议栈 fast-poll 获取。

## 保持不变

- BLE 业务及其无线独占/交接保护。
- QSPI DMA 修复、采集写 Flash、CRC 格式、原始波形先于特征值的流程。
- 在线监测开关、采样周期、节点状态上报、用户分组及参数继承。
- TI 协调器工程及 +5 dBm 设置；已确认其桥接支持普通 19 字节 UART ACK（短地址 2 字节 + ACK 17 字节），无需为此重烧 TI。
- 原 `Zigbee` 工程没有修改。新后端只接受普通 16 字节分块描述，不应混用仍使用时隙请求的旧节点固件。

时隙/多节点旧头文件及旧测试保留作为历史参考，不再被当前上传代码引用。`run-upload-scheduler.ps1` 现在转入当前单节点回归；Linux `make test` 使用新的单节点测试。

## 文件清单

节点业务修改：

- `Core/Inc/vibration_tx.h`
- `STM32_WPAN/App/app_zigbee.c`

后端业务修改：

- `gateway-core/include/vibration_tx.h`（与节点发送器同一份实现）
- `gateway-core/include/vibration_gateway.h`
- `gateway-core/src/main.c`（去掉调度初始化和排队状态）

验证修改：

- `gateway-core/Makefile`
- `tests/run-vibration-tests.ps1`
- `tests/run-upload-scheduler.ps1`
- `tests/test_single_upload.c`（新增）
- `tests/test_vibration_gateway.c`
- `tests/test_fusion_vibration_store.c`、`tests/stubs/stm32wbxx_hal.h`（补齐现有 Flash 统计/DMA 接口的主机模拟）
- 节点 `tests/verify-zigbee-migration.ps1`

修改前文件备份：`C:/Users/Administrator/STM32-Projects-1.91/power-backups/20261013-single-node-upload`，其中 `node` 和 `gateway` 分别保存对应原文件。

## 验证与上板

已通过协议编解码及故障回归（丢片、丢 ACK、重复片、损坏片、接收器重启、数据库失败），单节点状态机测试，实际 UART/数据库适配器测试（400 毫秒延迟 ACK 和缺片补传）、Flash/特征值/参数回归。BLE 23 个函数及 QSPI 外设保持检查通过。

节点 Debug、Release 均编译成功，并生成 ELF、HEX、BIN。后端 Linux x86_64 静态交叉编译通过；该二进制仅用于编译验证，不是用户 ARM 网关的部署程序。

上板时重新烧录当前 BLE/Zigbee 节点固件，在网关 `gateway-core` 目录重新运行 `make` 并重启 `./gateway-server`。TI 协调器不用改。建议先只启用一个节点的在线监测测试，不必因本次修改清数据库或擦除网络持久化。
节点启动标记：`ble-zigbee-single-upload-20261013`。

尚未进行实际无线硬件上传测试；主机仿真耗时不等于真实 Zigbee 上传速度。
