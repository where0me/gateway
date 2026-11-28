# TI CC1352R1 协调器

本目录是原工作工程 `zc_cc1352R_coord` 的源码副本，未改变协调器业务实现。目标 SDK 是 SimpleLink CC13xx/CC26xx **8.32.00.07**、TI-RTOS7，CCS 工程中保留对应产品配置和编译器设置。

1. 安装对应 SDK、CCS 和工程指定工具链。
2. 在 CCS 中导入本目录的 `.project/.ccsproject`，工程名保持 `zc_cc1352R_coord`。一个 workspace 中不要同时导入两份同名工程；原目录可以保留，导入副本时选择另一 workspace。
3. 检查 SDK 安装变量 `COM_TI_SIMPLELINK_CC13XX_CC26XX_SDK_INSTALL_DIR`，运行 SysConfig 并重新构建。
4. 使用 `targetConfigs/CC1352R1F3.ccxml`，按实际 XDS110/硬件连接下载。

当前桥接串口为 **921600、8N1**，输出二进制协议。引脚/RF 配置以 `zc_genericapp.syscfg` 和生成的板级配置为准。`PORTING_NOTES.md` 是早期移植记录，其中初始 115200 的描述不作为当前波特率设置。

`default/syscfg` 中保留已有生成源码作为基线；`default` 里的 makefile/对象/固件不提交。CCS 需根据本机 SDK 路径重新生成构建文件。

本次整理只复制源码，不清除原板网络、不修改原工作工程，也不执行硬件烧录。
