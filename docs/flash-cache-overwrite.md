# 节点Flash缓存覆盖

2026-10-26。仅修改 `STM32WB55RGV6-BLE-Zigbee` 双模节点工程。

`Core/Src/zigbee_vibration_store.c` 将槽位选择改为严格的空闲、已完成、最早未完成三级优先级。最早按持久化sequence的回绕安全比较选择。覆盖前持久化无效标志并读回验证，避免断电后出现带有效旧头的残缺波形。

`STM32_WPAN/App/app_zigbee.c` 开始使用上传批次前设置RAM保护，WAIT/休眠、续期、传感器校验、波形和特征确认均保留；所有上下文清理由 `ZbApp_ClearDeferredUpload` 统一释放。没有改节点/协调器/后端之间的协议或现有Flash格式。

`custom_app.c` 打印淘汰的旧批次、替代批次和槽位，并区分无安全槽位与实际Flash读写准备失败。覆盖日志只在节点串口打印。

`tests/run-flash-overwrite-tests.ps1` 编译真实存储实现，验证优先级、16槽连续淘汰、受保护波形/元数据不变、序号回绕、失败与断电恢复，并运行现有存储、元数据、异步DMA和清理回归。`run-reliable-control-tests.ps1` 同时验证实际恢复函数释放上传保护。

修改前源码位于 `out/overwrite-before-20261026/`；固件包位于 `out/cache-overwrite-20261026/`。节点工程内 `FLASH_CACHE_POLICY.md` 有使用说明。保留下载后首次启动清空外部Flash的既有功能；没有修改单独的Zigbee工程、网关或协调器。
