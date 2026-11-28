# MQTT 接入前本地源码存档（2026-10-20）

这份存档覆盖当前网关前后端、融合节点及 TI 协调器。仅创建本地 Git 提交，不推送远程，不修改业务逻辑，不安装或启动 MQTT 服务。

## 对应版本

- 网关前后端：本文件所属的 Git 提交，仓库 `C:/Users/Administrator/Documents/Codex/Zigbee-Gateway`。
- 当前融合节点：`0f4254f83cbdfa1f39cec768f878964277da2988`，仓库 `C:/Users/Administrator/STM32-Projects-1.91/STM32WB55RGV6-BLE-Zigbee`。
- TI 协调器：`8feddf822d1fbf48cd3a64a2893027c683642ee3`，仓库 `C:/Users/Administrator/workspace_ccstheia/zc_cc1352R_coord`。

## 存档内容与边界

保留当前待提交的源代码、项目配置、测试及说明文档，包括排队上传、双唤醒原因、上传日志及数据去重、前端趋势图、协调器信道操作等现有修改。

遵守各仓库现有忽略规则，不强制加入运行数据库、依赖目录、临时文件或编译产物；不删除这些本地文件。该提交不是数据库或设备 Flash 的备份。

网关本机运行配置 `gateway-core/gateway_config.json` 被现有规则忽略，未强制纳入 Git；此时读取到的值如下，供恢复环境时参考：

```json
{
  "serial_path": "/dev/ttyACM0",
  "baud_rate": 921600
}
```

部署模板 `deploy/rk3506/gateway_config.json` 正常跟随源码提交；本机运行配置与部署模板不必相同。

后续进行 MQTT 修改时，以这三个仓库的对应提交作为共同基线。
