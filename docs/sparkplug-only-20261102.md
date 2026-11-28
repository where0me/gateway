# 仅保留 Sparkplug B 上传

网关MQTT上传协议固定为Sparkplug B。前端已删除协议选择框，始终显示分组ID、Sparkplug B协议、Protobuf数据格式和发送完成状态。采集结果及完整振动原始数据分别编码为同一设备DDATA主题下的Protobuf消息；波形仍为XYZ有符号int16小端字节，Bytes测量项保持不变。

后端已移除旧JSON批次发布、独立二进制主题发布以及对应的QoS 1/PUBACK等待逻辑。Sparkplug Birth、Death、Rebirth、历史补发及本地数据完整性检查继续使用现有实现。数据消息采用QoS 0，发送完成表示写出网络连接，不表示服务器业务入库。

## 配置与历史数据

旧配置缺少`protocol`或使用`json`时，加载后使用`sparkplug_b`，缺少`group_id`时使用`factory01`。设置接口拒绝显式选择JSON；成功保存时写入Sparkplug配置并删除旧`topic_prefix`。账号密码及其他业务配置保留。

历史SQLite表及快照保留。表中的`json_payload`、`json_acked`等名称仅用于兼容旧数据库，快照在发布时编码为Protobuf，不是JSON格式MQTT消息。旧JSON目标不再发送，不占用Sparkplug队列配额；当前数据库中的完整批次按Sparkplug目标整理。已有Sparkplug目标不会仅因内部主题提示变更而生成重复修订。

## 更新与运行

本次只需更新网关后端及Web前端，无需烧录节点或协调器。在虚拟机停止旧后端后执行：

```sh
cd /mnt/hgfs/Zigbee-Gateway/gateway-core
make CC=gcc
ZIGBEE_CONFIG_DIAG=1 ./gateway-server
```

开发前端继续使用`npm run dev`并刷新页面。板端仍通过`deploy/rk3506/build-release.sh`生成ARM部署包；本次检查包里的`gateway-server-linux-x86_64`只供Linux虚拟机使用。

MQTTX订阅示例：`spBv1.0/factory01/+/gw001/#`。继续使用原Sparkplug Protobuf解码设置。

## 文档及图片

系统组成图更新为`out/document-review/RK3506系统组成图-SparkplugB.png`，MQTT链路仅标注Sparkplug B与Protobuf。

设计文档概述中的上传说明建议改为：网关通过MQTT采用Sparkplug B协议向服务器发布采集结果及完整振动原始数据。

设计目标中对应条目建议改为：支持通过MQTT采用Sparkplug B协议向服务器发布节点状态、采集结果及完整振动原始数据。

系统组成中对应说明建议改为：MQTT服务器负责接收和分发网关发布的Sparkplug B消息，可采用EMQX等MQTT Broker；订阅端依据Sparkplug Protobuf定义解析采集结果及振动原始数据。

## 验证

- 配置迁移、API拒绝JSON且不改变文件、密码管理、独立连接测试及队列持久化测试通过。
- 旧JSON队列配额隔离、内部主题变化不制造重复修订测试通过。
- 本地TCP验证所有发布主题为`spBv1.0/...`，DDATA为Protobuf，包含结果与完整波形。
- Sparkplug生命周期、历史补发、离线隔离及重连测试通过；官方Eclipse Tahu Protobuf定义独立解码验证240006字节波形逐字节一致。
- 前端MQTT设置测试、lint、生产构建通过；完整Linux后端以`-Wall -Wextra -Werror`构建通过。

未更新运行中的网关，也未执行本轮现场EMQX联调。修改前相关文件保存在`out/sparkplug-only-before-20261102/`。
