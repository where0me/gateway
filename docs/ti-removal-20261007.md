# TI 协调器删除流程修复

本次不修改 ST 节点固件、前端、数据库结构或 SDK 安装目录。TI 发射功率保持 +5 dBm。

## 修复内容

1. TI `ZDO_LeaveCnf` 对子节点原本只调用内部清理，不向应用转发确认；原桥接仅处理 `LeaveInd`，因此漏掉主动删除的完成路径。现在成功的子节点确认和非重入远端通知都通过栈任务完成清理及结果上报。失败的确认不清除节点状态。
2. 原桥接固定报告 `NV=FF, complete=0`。现在在官方清理前开始审计 NV 写入，清理对应 TC 记录，调用官方网络状态保存，并逐块读回比较；缺失记录与读取失败明确区分。不会手工重置 NWK 帧计数。
3. 仅匹配待删除槽位且 TC/APP/NV 全成功才发完成标志。30 秒没有结果会打印等待超时，保留关联；最多八个未结束槽位。原后端“节点下一次活动时发送离网”的行为保持不变。
4. 后端增加提交、确认/通知、清理、超时诊断，仍严格检查 14 字节结果的三个状态和完成标志。

`LeaveCnf`、`LeaveInd`、协调器清理完成均不等于节点自身 Flash 已擦除。需检查节点原有清理日志。NV 错误在本次启动期间锁存，避免重复事件掩盖已失败的写入；排查供电/Flash 后再重启重试。当前不是掉电恢复事务日志，也不保证 UART 完成帧丢失后自动恢复。

## 修改文件

TI 工程 `C:/Users/Administrator/workspace_ccstheia/zc_cc1352R_coord`：

- `Application/gateway_bridge.c`
- `Application/StartUp/main.c`
- `Stack/zdo/zd_app.c`
- 新增 `Application/gateway_removal.c`、`Application/gateway_removal.h`
- `PORTING_NOTES.md`

后端：`gateway-core/src/main.c`。测试：`tests/test_ti_removal.c`、`tests/ti-removal-stubs/`、`tests/run-ti-removal.ps1`。
原 TI 文件备份位于 `C:/Users/Administrator/workspace_ccstheia/cc1352r-backups/20261007-172433-removal`。

## 验证与烧录

- TI CCS default 完整编译/链接成功，0 错误；唯一警告是 SysConfig 版本替代。
- 实际清理模块通过 9 组模拟 NV/RTOS 故障注入测试。
- 后端 main.c 通过 x86_64-linux-musl 编译检查；未在本机运行 Linux 后端。
- 未烧录、未删除真实节点、未进行无线或掉电测试。

验收顺序：

1. 在 Linux 后端目录执行 `make`，停止旧进程后运行新 `./gateway-server`。
2. 烧录 TI 的新 `.out`，选择仅必要扇区，**不要全片擦除网络 NV**；节点不重烧。
3. 先用已正常通信的测试节点，进入休眠后点击删除。应显示等待活动，而非立即完成。
4. 等节点自然唤醒，依次检查离网提交、子节点 Leave 确认或远端通知、TC/APP/NV 成功、删除任务结束。
5. 从节点串口另行核实本地清理；然后开放加入，验证重新入网与授权。
6. 如果仍失败，提供删除点击至唤醒后一整段双方日志，尤其 `TI离网处理结果` 和 `Zigbee清理`。不要通过全片擦除掩盖问题。
