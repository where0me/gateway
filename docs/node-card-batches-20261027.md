# 节点详情的等待和上传显示

此处记录的等待时回退历史数据的显示规则，已由 [按本轮采集显示](node-card-cycle-20261027.md) 取代。下文保留作为此前行为说明。

等待或排队时显示“等待数据上传”，卡片展示该节点最近一个原始波形和振动特征均完整的批次。下载原始数据、在线波形图均操作这个批次；没有完整批次时按钮保持禁用。

收到该节点的本批传感器数据、上传状态或窗口进度后，卡片切换到本批。尚未收到的数值显示 `--`，不会混入上批数据；两个按钮保持禁用，直到本批原始波形和振动特征均已入库。采集下一批不会打断上一批正在上传的显示。其他节点保持各自的历史数据。

后端 `/api/device/vibration/latest` 新增：

- 可选 `batch` 参数：按 MAC 和批次精确读取；查不到返回 404，不回退到最新批次。
- `upload_started`：区分空批次描述符和已开始收到的数据。
- `last_complete`：最新批次不完整时，附带最近完整批次及其同批传感器数据，支持页面刷新后恢复展示。

不修改数据库表结构，不改动节点或协调器固件、上传许可及排队策略。

验证：`tests/run-node-card-batch-tests.ps1` 使用真实数据库和实际 API 函数检查完整批次回退、指定批次读取、原始波形/特征分步完成及非法参数；前端 `node-details-state.mjs`、`node-details-refresh.mjs`、`node-card.mjs`、`node-waveform-download.mjs` 检查批次状态、实际 App 刷新流程、卡片按钮和下载绑定。已完成 Linux x86_64 后端及前端生产构建，未代替现场设备测试。

在虚拟机停止原后端后重新编译启动：

```sh
cd /mnt/hgfs/Zigbee-Gateway/gateway-core
make -B CC=gcc
ZIGBEE_CONFIG_DIAG=1 ZIGBEE_RADIO_DIAG=1 ./gateway-server
```

前端开发服务运行时会自动加载源码变更，刷新浏览器即可；若已关闭，在 `gateway-ui` 目录运行 `npm run dev`。使用打包页面时需重新构建和部署前端。
