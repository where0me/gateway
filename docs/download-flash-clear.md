# 节点下载后的外部Flash清理

> 2026-10-27更新：当前代码已改为逻辑失效旧采集记录，下面的整片擦除描述是2026-10-26版本的历史行为。当前操作与验证见 `logical-reset-applied-20261027.md` 及节点工程内 `DOWNLOAD_FLASH_CLEAR.md`。

2026-10-26，仅修改实际双模节点工程 `C:/Users/Administrator/STM32-Projects-1.91/STM32WB55RGV6-BLE-Zigbee`。

工程内 `DOWNLOAD_FLASH_CLEAR.md` 给出下载操作及实现细节。核心行为：每次完整下载固件后，首次运行物理擦除并验证全部外部Flash；普通复位、断电上电和休眠唤醒保留缓存。重复下载相同固件也清空，不依赖版本变化。

改动：

- `Core/Inc/download_reset.h`：可隔离测试的清理事务，先擦除验证全部容量，再提交一次性完成标志。
- `Core/Src/main.c`：在CPU2/协议栈启动前执行清理，提供UART进度、HAL标志页提交及失败停止。
- `STM32WB55RGVX_BLE_ZIGBEE_FLASH.ld`：专用4 KiB页位于 `0x0806F000`，不与应用代码和 `0x08070000` 起的网络NVM重叠。
- `STM32WB55RGV6-BLE-Zigbee Debug.launch`：移除内部Flash的mass_erase；保留完整ELF下载、禁用增量、Connect under reset和校验。

标志页初始内容随完整ELF/HEX一起下载；清理成功后单独擦除该页并写入DONE，保证重复烧录同一个镜像仍然触发。掉电中断、擦除/读回/提交失败不进入正常采集；下次启动重试。整个8 MiB外部Flash都被擦除，包括16个采集槽位以外的空间。

`tests/run-download-clear-tests.ps1` 验证完整清理事务、相同镜像重下、普通启动保留、末字节读回、容量边界、中断重试，并抽取真实 `DownloadReset_Commit` 验证标志页地址、擦除后写入、HAL失败和CPU2保护。Debug和Release构建通过，原有RWX链接段提示仍在。没有操作真实板卡。

源码修改前备份：`out/download-clear-before-20261026/`。新节点镜像和共享启动配置：`out/download-clear-20261026/`。本次不改协调器和网关，也不包含前面讨论的缓存循环覆盖或旧批次补传触发修复。
