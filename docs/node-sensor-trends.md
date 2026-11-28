# 节点详情：最近 30 次采集趋势

在温度、数字麦克风、超声麦克风和磁场右侧点击曲线图标，右侧展示对应节点的变化趋势。
再次点击当前按钮或图表右上角关闭按钮可收起；选择其他节点/指标会切换图表。
节点卡片宽度按原列表列宽计算，不随趋势图开关变化。约400像素宽的小图紧邻所选卡片右侧，
其他节点按剩余空间换行；窄屏空间不足时图表换到该卡片下方，不拉伸卡片。
磁场同时显示 X/Y/Z 三轴。图表从旧到新排列，横轴为采集顺序；悬停或键盘聚焦数据点查看数值。

前端每 3 秒读取一次网关已有的历史记录，不会因此向 Zigbee 节点发送采集/唤醒命令。
切换指标、离开页面后会取消请求和定时器。离线节点也可以查看数据库中已有的历史。

## 后端接口

`GET /api/device/collection_history?mac=<MAC>&metric=<指标>`

指标可选 `temperature`、`digital`、`ultrasonic`、`magnetic`。
响应包含 `status`、`mac`、`metric`、`limit: 30`、`count` 和按时间升序排列的 `data` 数组。
每个记录包含 `batch_id`、`timestamp`、`temperature`、`mag_x/y/z`、`digital_db`、`analog_db`。
缺失值为 null，不会用 0 代替。仅查询所选指标非空的批次，最多 30 条。
时间是现有数据库记录的网关上报接收时间，不是新增加的节点采样时钟。

复用 `collection_values` 表的 `(mac,batch_id)` 唯一键，对同一批次的重复上报只显示一个点。
不依赖页面内存，不要求新的 Zigbee 协议字段，也不改变现有温度/声级按时间查询接口。
自动添加 `(mac,timestamp)` 索引；不清空现有数据。

## 验证和部署

- `gateway-ui`：`node tests/sensor-trend.mjs`、`node tests/node-card.mjs`、`npm run build`。
- 数据库：`tests/test_collection_history.c` 使用内存 SQLite，覆盖四个指标、30条上限、批次去重、空值/零值/负数、节点隔离及删除后的历史清理。
- `gateway-core/Makefile` 的 `make test` 包含上述数据库用例。
- `gateway-ui/tests/sensor-trend-preview.html` 是独立的模拟数据预览页，不连接实际数据库；不属于生产界面。
- 前后端都需更新。Linux 上重新编译并重启 `gateway-server`；前端重新构建/更新，开发模式直接刷新页面。
- ST 节点和 TI 协调器固件不变。
