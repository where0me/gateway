# 节点 DMA 异步采集改造（2026-10-03）

## 范围和恢复

本次只修改纯 Zigbee 节点工程 `C:\Users\Administrator\STM32-Projects-1.91\Zigbee`，不修改协调器、后端业务或前端。网关仓库新增/调整存储测试及本说明。

保留用户修改后的 Zigbee.ioc、生成器设置与工程配置；从 `d31910b` 恢复生成器覆盖的业务代码，再合并 DMA 初始化和中断。原有网络恢复握手、采样配置、节点删除、Flash 记录失效、官方协议栈低功耗管理和 +6 dBm 发射设置保留。

改造前存档标签：`snapshot/pre-async-vibration-20261003`。网关、节点、协调器的独立 Git bundle 位于 `out/pre-async-vibration-20261003/`。生成后的中间状态另存为节点标签 `snapshot/ioc-dma-generated-20261003`（不是可运行的业务基线）。部署前文件备份在 `out/async-vibration-20261003-192204/`。

## 实现

- SPI1 FIFO 批量读取改为 DMA 启动后返回；完成中断只记录状态、释放片选，由任务解析数据。控制寄存器短读写仍同步。
- 保持 13334 点 SRAM 环形缓冲；SPI 接收使用交替缓冲，避免复用正在传输的缓冲。
- QSPI 使用 DMA1 Channel 6，单页异步发送；再用状态自动轮询中断等待 NOR 内部编程完成。另一个页缓冲接受后续数据，等待 Flash 时任务可以继续服务 FIFO。
- 不在 DMA 完成中断中进行 Flash 写入、特征计算或网络业务。
- 采样结束后继续排空环形缓冲和最后一页；写入完成后才允许提交记录和释放低功耗锁。错误/超时取消 DMA，沿用“不上传不完整波形”规则。
- 不改变 Flash 记录格式、无线上传格式、采样参数或网络密钥。

主要文件：`Core/Src/main.c`、`stm32wbxx_hal_msp.c`、`stm32wbxx_it.c`、`qspi_nor.c`、`zigbee_vibration_store.c`；对应头文件；`Middlewares/Sensors/Vibrometer/vibrometer.c/.h`；`STM32_WPAN/App/sensor_app.c`、`app_zigbee.c`。

## 已验证

- 节点 Debug/Release 编译：0 errors，0 warnings。
- 异步存储模拟测试：13334、26667、40001 点逐字节读回一致；跨页处理、DMA 源缓冲不被提前复用、尾页排空、失败后取消及重新采集通过。
- 原有 Flash 逻辑重置回归测试：失败重试、重启、保留物理数据但失效旧记录、新采集通过。

尚未烧录或进行实板吞吐测试。DMA 不会缩短 NOR 内部编程时间；真正收益来自 FIFO 服务与 Flash 写入等待重叠。不能仅凭编译/模拟测试宣称 1.5s 已修复。

## 实板测试

无需继续改 IOC，也无需 mass erase。烧录 `out/async-vibration-20261003/Zigbee-Debug.hex` 应用固件；保留 CPU2 Zigbee 协议栈和网络 NVM。

启动版本应为 `async-vibration-20261003-a`；诊断应为 `VIB-DIAG: async-v2`。依次测试 0.5s、1s、1.5s，再连续运行至少三轮 1.5s。

1.5s 预期：`samples=40001 target=40001`，`overrun=0 spi_err=0 flash_err=0 buffer_full=0 timeout=0`，原始数据应为 240006 字节（压缩前）。检查后端最终完整校验入库，并比较 `ring_peak`、`gap`、`service`。本版异步等待不计入任务忙时间，日志耗时不能与同步版本直接等同比较。

如果失败，请保留同一批次的 `VIB`、`VIB-DIAG`、`VIB-FLASH` 及后端上传结果，以判断是 DMA 传输错误、FIFO 服务延迟还是 Flash 长期吞吐不足。

不要再次直接生成代码覆盖业务实现；若需要继续改 IOC，应先存档，再做生成差异合并。
