# STM32WB BLE + Zigbee 节点 Flash 协调修复（2026-10-26）

目标工程：`C:/Users/Administrator/STM32-Projects-1.91/STM32WB55RGV6-BLE-Zigbee`。
本轮修改 CPU1 应用和它使用的 EEPROM Flash 驱动。协调器、后端、前端和 CPU2 固件未在本轮修改。

## 修改内容

- 访问 Flash 前取得 SEM2，取得失败最多等待 1000 ms；等待时中断保持开启。
- 每次 64 位写入、每页擦除分别检查 SEM6、PESD/BSY/CFGBSY 并取得 SEM7，在短临界区中调用同步 HAL；操作完成马上释放 SEM7。两次取得之间至少间隔 1 us（CPU1 最高 64 MHz）。不再启用项目未提供处理程序的 EOP 中断。
- 擦除前后配对发送 `SHCI_C2_FLASH_EraseActivity(ON/OFF)`，操作失败也走清理路径。
- 检查 HAL 结果、擦除错误页和读回内容。CPU1 数据缓存打开时，写入后先失效数据缓存再校验。
- 仅允许访问现有 NVM 区域 `0x08070000–0x0807FFFF`。EEPROM 格式、保存内容、网络密钥及计数器导出流程未改变。
- Flash 操作期间用独立低功耗标志阻止 STOP2；拒绝 ISR、屏蔽中断上下文和嵌套调用，退出时释放本驱动取得的资源。
- `EE_Init(0)` 返回擦写错误时不再直接格式化。保留原有 `EE_STATE_ERROR` 时的格式化分支；这不是对所有损坏数据的恢复保证。

参考 [ST AN5289，第 4.7.1 节及图 10](https://www.st.com/resource/en/application_note/an5289-how-to-build-wireless-applications-with-stm32wb-mcus-stmicroelectronics.pdf)，并对照本机 STM32CubeWB V1.23.0 的 `BLE_RfWithFlash/Core/Src/flash_driver.c`。本修复增加了该示例未检查的 HAL 返回值、读回校验和错误退出清理。

## 检查结果与边界

- Debug、Release 均编译链接成功。链接器仍报告项目已有的 RWX LOAD 段警告，本轮未修改链接脚本。
- `tests/run-node-flash-tests.ps1` 编译实际驱动代码，以主机 HAL/HSEM/SHCI 模型注入故障：SEM2/6/7 占用、各个 Flash 忙标志及检查间隙竞争、HAL 错误/超时、擦除通知失败、读回错误、多页中途失败、嵌套调用、越界访问。检查清理后能继续使用驱动。
- 同一脚本编译实际 `ee.c` 和驱动：20,000 次更新，56 次页面擦除，多次重建 EEPROM 状态，最终全部数据读回一致。主机模型不等于真实 CPU2 无线时序测试。
- 配置诊断开/关和网络恢复的既有主机回归测试通过。
- 没有替用户烧录，也没有完成板上无线测试；不能据此宣布间歇失联已解决。仲裁等待有上限；物理 Flash 故障导致的处理器停顿不由这些主机测试覆盖。

修复前四个源文件的原样副本在 `out/node-flash-before-20261026/`，包含本轮之前已有的诊断改动。没有重置或覆盖用户的其他修改。

## 只给 9D2C 烧录并复测

1. 使用 Debug 固件 `out/node-flash-fix-20261026/STM32WB55RGV6-BLE-Zigbee.elf`，或在 CubeIDE 中正常构建、下载当前 BLE + Zigbee 工程的 CPU1 应用。不要执行整片擦除或重新烧录 CPU2；本次需要保留现有网络记录。包内 HEX 只包含 CPU1 应用加载内容。
2. 启动串口应出现：

   ```text
   ZIGBEE: build ble-zigbee-flash-arbitration-20261026 | <编译时间>
   ZIGBEE-NVM: SEM2/SEM7 arbitration; verified writes; per-page erase
   ```

3. 后端继续运行配置诊断：

   ```sh
   cd /mnt/hgfs/Zigbee-Gateway/gateway-core
   ZIGBEE_CONFIG_DIAG=1 ./gateway-server
   ```

4. 先保持 9D2C 在线监测关闭，记录至少 10 分钟并覆盖一次待机睡眠后的唤醒。重点对比 `persisted network saved` 前后，节点是否继续接受配置，后端是否继续收到该 MAC 的 `RX99`。待机时本来就有较长睡眠间隔，不把静默本身视为故障。
5. 再开启在线监测，连续观察至少三个采集周期（当前周期 300 秒），确认原始波形和特征完成上传，睡眠唤醒后仍能通信。最后可以重启 9D2C 一次，确认能够保留网络恢复。
6. 如果再现失联，保存从启动开始的节点串口和同时段后端日志，尤其保留保存前最后一次配置、首次查询超时及任何 `NVM write failed` / `NVM recovery failed` 信息。不要仅凭 `SUBMIT-END rc=00` 判断无线发送成功。

正常保存和配置查询仍采用原有日志格式；不为每个 64 位 Flash 写入增加串口打印，也不调整节点接收模式、轮询周期、重试预算或采集周期。
