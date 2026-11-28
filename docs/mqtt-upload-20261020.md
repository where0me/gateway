# MQTT 上传第一版（2026-10-20）

此文档保留第一版设计记录。自2026-11-02起JSON批次发布已移除，当前只使用Sparkplug B；现行配置、主题和发送语义见 [Sparkplug B 数据上报](mqtt-sparkplug-20261027.md)。

## 范围与已验证连接

第一版仅改网关后端、配置及测试。后续已接通真实前端设置，部署与接口见 [MQTT 设置](mqtt-settings-20261020.md)。节点、TI 协调器和蓝牙无需更新。

- Linux 网关到电脑 EMQX：`mqtt://192.168.137.1:1883`。
- MQTT 3.1.1，QoS 1，Retain=false。
- 网关用户 `gateway`；MQTTX 用户 `user1`。不同连接必须使用不同 Client ID。
- 本机运行配置 `gateway-core/gateway_config.json` 已开启 MQTT，使用用户提供的测试认证信息。此文件沿用 Git 忽略规则，不提交密码。
- 部署模板 `deploy/rk3506/gateway_config.json` 默认关闭 MQTT，密码留空，部署后自行设置或使用环境变量。

目前只支持已验证的本机/隔离局域网 MQTT/TCP。`mqtts://` 不会静默降级为明文，而是校验失败、停用 MQTT。正式公司公网部署需另接 TLS、证书校验与强密码/最小主题权限。

## 启动与 MQTTX

先停止旧的后端进程，再在 Linux 的 `gateway-core` 目录执行：

```sh
make
./gateway-server
```

不要清空数据库；新增表会自动创建，并扫描已有数据。配置读取位置仍为当前运行目录的 `gateway_config.json`。网页“设置 → MQTT 上传 → 保存并应用”立即生效；外部直接修改文件后需重启后端。凭据也可通过 `GATEWAY_MQTT_USERNAME`、`GATEWAY_MQTT_PASSWORD` 覆盖，日志不打印密码。Linux 配置文件建议仅允许运行用户读写（0600）。

MQTTX 保持 `user1` 连接，新增 QoS 1 订阅：

```text
zigbee/gw001/#
```

结果主题（MAC 去掉冒号）：

```text
zigbee/gw001/nodes/0E54EF0025E18000/collection
```

有波形时的主题：

```text
zigbee/gw001/nodes/0E54EF0025E18000/batches/<batch_id>/revisions/<revision>/waveform
```

结果用 JSON 查看；波形用 Hex 查看，不能作为 UTF-8 文本解析。仅订阅此前的 `zigbee/test` 不会收到正式数据。账号必须具有相应主题的发布/订阅权限。

## 结果及二进制格式

一条结果 JSON 包含：

- `schema_version=1`、`gateway_id`、原格式 `mac`、`batch_id`。
- `message_id`、`revision`：重试同一任务不变；真正的数据修正生成新修订。QoS 1 允许重复消息，接收程序按这些标识幂等处理。
- `collection_complete=true` 表示网关本地已具备本批所需数据，**不是 EMQX 或业务数据库已经保存本批的证明**。
- `received_at` 是网关接收记录的 Unix 秒时间；`collected_at=null`，不把网关时间冒充同步后的节点采集时间。
- `measurements.temperature_c`、`magnetic_ut.{x,y,z}`、`digital_mic_db_spl`、`ultrasonic_mic_db_spl`。
- `vibration.available`、`sample_rate_hz`、`sample_count`、`features.{x,y,z}`。特征依次为 `rms_g`、`peak_g`、`peak_to_peak_g`、`kurtosis`、`dominant_frequency_hz`；数据库中的千倍整数在导出时还原。
- `waveform.available`、`status`、`encoding`、`scale_g_per_lsb`、`bytes`、`crc32`、`topic`。没有波形时 `available=false`、`status=not_in_batch`、`bytes=0`、CRC/topic=null。

波形 MQTT 载荷为数据库中已解压的 XYZ 原始字节，**没有额外私有包头**：

```text
X0(int16 LE), Y0(int16 LE), Z0(int16 LE), X1, Y1, Z1, ...
```

长度恰好 `sample_count × 6`；以有符号小端 int16 解码，当前量程按 `0.000488 g/LSB` 转换。不是 Zigbee Rice 压缩数据，也不是 CSV/JSON。JSON 的 `waveform.topic` 关联二进制消息，主题包含修订号，不会把同批后续修正与先前波形混淆。CRC32 使用 IEEE/ISO-HDLC（zlib CRC32）对全部二进制载荷计算。

已验证最大当前窗口 40,001 点、240,006 字节可经本机 EMQX 完整接收。换服务器时仍需核对其报文限制。

## 完成判定、历史与 N 次策略

MQTT 不重新控制或计算 N，继续沿用节点现有策略。只发送网关实际已获得的波形，不要求节点补发本来不上传的波形。

接收线程记录每批开始/上传/波形到达/最终特征/完成的证据；快照保存最近下发的传感器启用策略，后续修改不会改写旧批的判定。温度、磁场及按该策略启用的麦克风必须已有本批数据。特征必须是真正的 15 个有效数值，不能是待计算的 INT32_MIN 占位符。

有波形：长度必须完整。已开始接收波形但尚未完整保存时不能当成“本批不需要波形”。无波形的新批：需要当前节点流水线的最终特征通知或完成通知；不含振动的小数据批需完成通知。当前融合节点在应上传波形时，只有波形完成后才发送最终特征，本版据此判断收尾，无需改节点协议。启用策略是最近提交的配置而不是节点逐字段回读，不新增端到端采样配置验证；无法满足条件的记录保留待核验。

已有历史中的完整波形＋有效特征＋齐全标量可补发。历史没有每批策略时保守要求全部标量；没有波形又没有完成依据的历史批无法分辨“按 N 跳过”与“失败未传完”，标为 `legacy_completion_unknown`，不伪造完整。源记录仍保留，不删除或丢弃。缺少标量、未完成特征或残缺波形也不会冒充完整发布。

## 可靠性与资源边界

- 同一 SQLite 保存点中快照 JSON 和二进制数据，原子加入 `mqtt_outbox`；接收字段更新会原子标记待整理批次，不依赖容易被覆盖的 WebSocket 单条缓存。
- Mongoose MQTT 网络对象仅由主事件线程操作；串口线程只更新本地数据库。每 200ms 最多整理一个批次，避免一次扫描/复制全部历史长时间占用数据库锁。
- 一次只发一条 QoS 1 消息：先 JSON，收到对应 PUBACK 后再发波形。两部分全确认才标批次上传完成。跳过波形的批只需 JSON 确认。
- PUBACK 包 ID 必须匹配当前连接的在途任务。连接失败、超时、进程重启、ACK 状态落库失败都不会删除任务；重新连接后重发未确认部分。
- 使用 Clean Session=true，依靠自己的 SQLite 队列跨连接恢复，不依赖 Broker 会话替网关保管待发送数据。重复投递仍可能发生，不能承诺 exactly-once。
- 默认连接超时 10 秒、PUBACK 超时 15 秒、Keep Alive 60 秒、重连退避 1～30 秒。及时处理 PINGRESP，未收到则重连。
- `max_pending_bytes` 默认 512 MiB；到限后不删除旧任务，而是暂停快照新任务、保留源数据，确认腾出容量后继续。源数据库仍会增长，需监控磁盘空间。
- 全确认后释放队列中的大载荷副本，保留摘要、身份和确认审计；不删除原有采集数据。SQLite 文件大小不保证立即缩小。
- 已快照但未确认的任务独立于 devices 的外键删除，删除源节点不会暗中丢弃这些上传任务。没有快照的源数据仍遵守原有节点删除行为。
- Broker、用户名、网关编号、主题前缀共同隔离目的地；换目的地不会误把旧目的地的在途确认标为新目的地成功。密码变更不触发全部历史重复上传。
- MQTT 断线不拖住节点，节点在网关本地保存成功后仍按原流程得到回复、休眠。

可在停止后端或使用只读 SQLite 连接时检查：

```sql
SELECT reason, count(*) FROM mqtt_batch_state GROUP BY reason;
SELECT id, mac, batch_id, json_acked, raw_acked, acked_at
FROM mqtt_outbox ORDER BY id DESC LIMIT 20;
```

第一阶段完成日志仅代表 EMQX 的 MQTT 确认，不代表 MQTTX 一定在线、收到或将数据长期入库。本版不增加服务器存储程序或业务完成回复。

## 验证

`tests/run-mqtt-tests.ps1`：配置校验、真实 SQLite、二进制不可变快照、历史判定、无波形批次、关闭传感器、重复数据、SQL/ACK 失败回滚、额度、重启恢复、错误包 ID、拒绝连接、丢 JSON 确认超时、丢波形确认断线、选择性重发与心跳。

`tests/run-mqtt-tests.ps1 -RealEmqx`：仅用内存测试数据库和 `zigbee/test/integration-<唯一号>/#` 合成主题，实际连接现有本机 EMQX，使用 gateway 发布、user1 订阅，核对 JSON、240,006 字节波形和 CRC、两次 PUBACK。不读写用户运行数据库，不重配或重启 EMQX。测试账号可用 MQTT_TEST_SUB_USER / MQTT_TEST_SUB_PASSWORD 环境变量覆盖。

原有分块传输、补传、Flash、排队和去重测试通过；后端已进行 Linux musl 交叉编译。尚需在实际 Linux 网关运行新版并用真实节点做端到端验证。
