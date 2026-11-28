# Sparkplug B 数据上报（2026-10-27）

本次实现网关到 MQTT 服务器的 Sparkplug B 上报链路。RK3506 或虚拟机作为 Edge Node，每个 Zigbee 传感器作为 Device。在线监测、采样配置继续通过现有前端操作；MQTT 接收 `Node Control/Rebirth` 请求，用于重新发布设备信息。

## 启用

在虚拟机里停止旧后端，然后执行：

```sh
cd /mnt/hgfs/Zigbee-Gateway/gateway-core
make CC=gcc
ZIGBEE_CONFIG_DIAG=1 ./gateway-server
```

前端继续运行 `npm run dev`，刷新页面。在“设置 → MQTT 上传”中：

1. 上传协议固定为 **Sparkplug B**，无需选择。
2. 服务器地址填写运行 EMQX 的电脑当前可达 IP，端口通常为 `1883`。
3. 填写该 EMQX 的 MQTT 用户名和密码。
4. 分组 ID 可填 `factory01`，网关 ID 可填 `gw001`；同组的不同网关使用不同 ID。
5. 打开启用上传，保存并应用。

自2026-11-02起仅保留Sparkplug B。旧配置缺少协议字段或仍写有`json`时，加载后自动使用`sparkplug_b`；缺少分组时使用`factory01`。保存配置会写入规范值并移除旧主题前缀，新设置请求不能选择JSON。旧JSON目标的队列记录保留为历史记录，不再发送，也不占Sparkplug队列配额；本地完整批次向Sparkplug目标重新整理。更换服务器、账号、网关或分组会建立独立上传目标。

本轮只更新网关后端及前端，无需重新烧录 STM32 节点或 TI 协调器。板端发布包仍使用 `deploy/rk3506/build-release.sh` 在原交叉工具链中编译。

## 消息与状态

以分组 `factory01`、网关 `gw001`、节点 `0E:54:EF:00:25:E1:80:00` 为例，Device ID 是去冒号后的完整 MAC：

| 消息 | 主题 | 内容 |
|---|---|---|
| NBIRTH | `spBv1.0/factory01/NBIRTH/gw001` | 网关信息、bdSeq、重发信息控制项 |
| DBIRTH | `spBv1.0/factory01/DBIRTH/gw001/0E54EF0025E18000` | 该设备的全部测量项、类型、单位和别名 |
| DDATA | `spBv1.0/factory01/DDATA/gw001/0E54EF0025E18000` | 完整批次的测量值、振动特征，以及单独的波形 Bytes 消息 |
| DDEATH | `spBv1.0/factory01/DDEATH/gw001/0E54EF0025E18000` | 网关现有在线判定认为节点离线或节点被移除 |
| NDEATH | `spBv1.0/factory01/NDEATH/gw001` | 连接的 MQTT Will，服务器在连接丢失时发布 |
| NCMD | `spBv1.0/factory01/NCMD/gw001` | Boolean `Node Control/Rebirth=true`，也支持 NBIRTH 声明的别名 `1` |

先获得 NCMD 的 QoS 1 订阅授权，再发送 NBIRTH；设备先 DBIRTH，后 DDATA。断线重连会重新声明网关和在线设备。`seq` 从 NBIRTH 的 0 开始，按消息递增并在 255 后回到 0；Rebirth 重新从 0 开始。`bdSeq` 通过 SQLite 持久化，每次新连接增加，NBIRTH 与本次 CONNECT Will 匹配。

节点正常休眠不直接触发 DDEATH，沿用现有网关离线判定。离线节点的积压保留在本地，重新在线后先 DBIRTH 再补发；不会阻塞其他在线节点。历史批次标记 `is_historical`，最新批次作为当前值。DBIRTH 从最近的完整本地快照初始化，缺少的值为 `is_null`，不伪造为 0。

时间使用 UTC Unix 毫秒。当前节点没有同步的采集时钟，测量时间沿用网关收到该批数据的时间；`Batch/ReceivedAt` 也明确表达此含义，不冒充节点实际采集时间。

本轮实现无需配置 Primary Host 的单 Broker 接入；未执行 Sparkplug TCK 认证。

## 测量项

- `Device/MAC`、`Device/Name`：稳定标识与显示名称。
- `Batch/Id`、`Batch/MessageId`、`Batch/Revision`、`Batch/ReceivedAt`：关联批次、修订及去重。
- `Batch/Complete`：现有批次完整性条件已满足；`Capture/Complete`：启用的传感器是否全部采到数据。
- `Temperature`、`Magnetic/X/Y/Z`、`Sound/Digital`、`Sound/Ultrasonic`。
- `Vibration/SampleRate`、`Vibration/SampleCount`，以及各轴 RMS、Peak、PeakToPeak、Kurtosis、DominantFrequency。
- `Waveform/Available`、`Waveform/Encoding`、`Waveform/Scale`、`Waveform/CRC32`、`Waveform/Size`、`Waveform/Samples`。

波形为独立 DDATA 中的 Bytes 测量项。它保留原 `XYZ int16 little-endian` 字节，按 `Batch/MessageId` 和 `Batch/Revision` 与测量结果关联。没有将大波形嵌入 DBIRTH；DBIRTH 声明 Bytes 类型并给出空值。

## QoS 与本地队列

Sparkplug 的 NBIRTH、DBIRTH、NDATA、DDATA、DDEATH 使用 **QoS 0、retain=false**，CONNECT 的 NDEATH Will 使用 **QoS 1、retain=false**。不能继续等待不存在的 DDATA PUBACK。

因此 Sparkplug 模式在数据写出网络连接后记录“发送完成”，不承诺 Broker 已接收或服务器已入库。发送未完成、连接断开、编码失败时本地队列保留。已经写出的 QoS 0 消息仍可能因后续链路故障丢失；需要业务入库保证时还需接收端的应用确认设计，不能以此发送状态代替。原始采集记录继续服从现有数据库保留策略。

仅Sparkplug B参与发布；本地快照仍可采用JSON表示，以保持SQLite历史数据兼容，但不会作为JSON格式发布到MQTT。

## 在 EMQX 电脑上查看数据

EMQX 可继续作为 MQTT Broker。Sparkplug 是 Protobuf，MQTTX 的普通 JSON 视图不能直接把它显示成原有 JSON 结果。仓库提供基于官方 Tahu `.proto` 的调试订阅器：

```sh
cd integrations/sparkplug
npm ci --no-bin-links --ignore-scripts
```

Windows PowerShell 示例：

```powershell
$env:MQTT_USERNAME = 'gateway'
$env:MQTT_PASSWORD = '填写实际MQTT密码'
node monitor.mjs mqtt://127.0.0.1:1883 factory01 gw001
```

Linux 示例：

```sh
MQTT_USERNAME=gateway MQTT_PASSWORD='实际MQTT密码' node monitor.mjs mqtt://127.0.0.1:1883 factory01 gw001
```

订阅器先订阅网关消息，再请求 Rebirth，以便即使网关早已连接也能收到别名定义。输出可读的测量项；波形只显示字节数，避免在终端刷出大量二进制。此工具用于诊断，不执行数据库入库。

账号需允许发布网关组内 NBIRTH/DBIRTH/NDATA/DDATA/DDEATH/NDEATH，并允许订阅对应 NCMD；调试账号还需订阅消息及发布 NCMD。不要把 MQTT Dashboard 登录账号与 MQTT 认证账号混用。

## 验证

新增真实 TCP 测试覆盖 CONNECT Will、持久化 bdSeq、订阅授权、生命周期顺序、别名和类型、历史数据、波形字节、重发信息、离线积压隔离、重连，以及订阅被拒后不发布业务数据。另用官方 Tahu schema 和独立 protobufjs 解码器检查实际发出的消息。

```powershell
./tests/run-sparkplug-tests.ps1
./tests/run-mqtt-tests.ps1
```

独立解码检查需要在 `out/sparkplug-tests/decoder` 安装 `protobufjs@7`，并在 `out/sparkplug-tests/sparkplug_b.proto` 放置官方 schema（与 `integrations/sparkplug/sparkplug_b.proto` 相同）：

```sh
node tests/sparkplug-official-decode.mjs
```

调试订阅器还有独立 TCP 测试：在上述 decoder 目录安装 `mqtt@5` 后执行 `node tests/sparkplug-monitor.mjs`，校验认证、订阅、Rebirth 请求、别名解析与大波形输出。

前端 MQTT 设置测试、lint 和生产构建通过。实际学校网络中的 EMQX 连通性仍需现场验证。

依据：[Sparkplug 官方规范](https://sparkplug.eclipse.org/specification/)、[会话与设备生命周期规则](https://github.com/eclipse-sparkplug/sparkplug/blob/master/specification/src/main/asciidoc/chapters/Sparkplug_5_Operational_Behavior.adoc)、[Eclipse Tahu Protobuf 定义](https://github.com/eclipse-tahu/tahu/blob/master/sparkplug_b/sparkplug_b.proto)。仓库内 schema 保留上游 EPL-2.0 声明。
