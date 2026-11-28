# 入网与 Cluster v3（2026-10-22）

## 工程与恢复点

- 实际节点：`C:/Users/Administrator/STM32-Projects-1.91/STM32WB55RGV6-BLE-Zigbee`，保留 BLE GATT、连接参数及双模 CPU2 固件。**不是**旁边的纯 `Zigbee` 工程。
- 协调器：`C:/Users/Administrator/workspace_ccstheia/zc_cc1352R_coord`。
- 三个实际目标仓库均有修改前标签 `snapshot/pre-cluster-20261022`。离线 bundle 与恢复步骤见 `out/git-snapshots/pre-cluster-20261022/RECOVERY.md`；纯 Zigbee 工程也备份但不修改。
- 不覆盖数据库、不擦除网络、不刷写设备。回退应三端一起回退；先备份设备上的数据库/配置，再在新目录恢复源码。源码 Git 快照不等于设备数据库备份。

## 行为

1. 前端 POST `/api/device/scan` 提交请求。HTTP 202 仅表示请求已发送；协调器 `DEV_PERMIT_JOIN_IND` / 状态查询回应才是实际开放状态。
2. 开放 **254 秒**，到期自动关闭，不再无限续期开网。可提前点击“禁止节点加入”。协调器重启默认关网；后端重启通过查询恢复真实状态。
3. 节点完成 Zigbee 入网、协调器转发 IEEE Device Announce 后，后端立即登记设备并默认关闭在线监测。**无需第二次手动批准**。同一 IEEE 重复宣告不增加记录，不覆盖既有监测设置。
4. 设备已在列表中时，协调器异步查询 Active Endpoints → Simple Descriptor → Basic Model → HY 协议版本/能力。描述符可读与“私有采集协议就绪”是不同状态。只有版本匹配且 HELLO 后收到节点配置查询，才标为采集协议就绪。
5. 识别失败保留设备，可在 Cluster 详情点“重新识别”。只支持标准簇的设备显示“标准设备（无采集协议）”，不向它发送 HY 采集命令。
6. 停止/到期只关闭新设备入网入口，不清退设备，不停止现有采集，不清空列表。关闭边界稍晚到达的已接受入网宣告也会保留。
7. 用户明确删除节点的 Leave/撤销保护继续保留。被删除 IEEE 不因自动登记重新入库；“手动添加”可解除该 IEEE 的撤销保护，但**不会隐式开网**。
8. 列表每 3 秒同步数据库，识别信息持久化，刷新页面/重启后端仍在。网络开放状态超过 8 秒没有新观察则显示未知，不伪装已连接。
9. 短地址被另一个 IEEE 复用时，协调器清除旧端点缓存，后端原子解除旧地址绑定。旧设备记录、历史数据及用户监测设置保留，不再向旧地址下发控制。
10. 型号读取统一由协调器识别流程负责；后端收到重复入网宣告时只更新上线信息，不再追加旧的 `0x04` 型号查询。重复型号回复不重复写数据库、打印名称更新或推送名称通知；真实型号变化仍更新，用户自定义名称保留。

## 节点端点与簇

Profile `0x0104`，端点 `1`，Device ID 为 Temperature Sensor；Basic Model `HY-Sensor`，Manufacturer `HuaYang`，SW Build `3.0.0`。

| 服务端 Cluster | 用途 |
| --- | --- |
| `0x0000` Basic | 厂商、型号、软件版本、外部直流供电属性 |
| `0x0003` Identify | 标准 Identify 服务；本轮未增加专属指示灯效果 |
| `0x0402` Temperature Measurement | 标准温度属性（0.01 °C）、读取和协议栈的标准报告配置 |
| `0xFC10` HY Control | 私有协议版本、采集配置状态与控制命令 |
| `0xFC11` HY Data | 最近一次采集快照、原有批次/波形传输命令 |

不再把自定义采集命令塞进 `0x0402`，也不冒用其他厂商的 manufacturer code。私有簇目前使用普通 ZCL 帧头；这些 ID 是本项目约定，不表示已获 CSA 分配。私有采集端点由来自网络地址 `0x0000` 的 v3 HELLO 学习，不硬编码必须为端点 8。

温度标准值来自真实温度传感器，未得到有效读数时用 `0x8000` 未知值。未选择 HY 采集主机时，节点在 Zigbee 工作时约每 60 秒更新标准温度属性；BLE 占用期间不抢传感器/无线资源。标准读取可能返回上次更新值。标准报告依赖第三方协调器完成绑定和 Configure Reporting。

### FC10 只读属性

| ID | 类型 | 含义 |
| --- | --- | --- |
| 0 | uint16 (`0x21`) | 协议版本 3 |
| 1 | bitmap16 (`0x19`) | 能力位：bit0 温度，bit1 磁力，bit2 数字麦克风，bit3 模拟麦克风，bit4 振动 |
| 2 | bool | 在线监测启用状态 |
| 3 | uint32 | 采样周期，秒 |
| 4 | bitmap8 | 高频传感器使能掩码，沿用旧采样协议（不是上述能力位图） |
| 5 | enum8 | 当前监测状态 |
| 6 | bool | 是否已有有效采样配置 |
| 7 | uint16 | 振动采样率，Hz |

FC11 只读属性：0=批次 uint32；1=有效字段掩码 bitmap8；2/3/4=磁力 XYZ int16（0.1 µT）；5/6=数字/模拟声级 uint8（dB SPL）；7=振动点数 uint32。必须结合有效掩码解释快照，默认 0 不等于已完成采样。

HELLO：FC10，客户端→服务端，命令 `0x00`，payload `03 00`。
原 `0x0B/0x0C/0x99/0x8D` 等配置/状态消息走 FC10；`0x86..0x8C`、`0x92..0x95`、`0x0D` 走 FC11。原 UART 批次内容及 V2 波形内容不变；**v3 是 ZCL 协议版本，不是把 V2 波形格式改为 v3**。

## 协调器与后端串口增量

仍使用原 `FE CMD LEN PAYLOAD XOR` 格式，整数小端：

| 命令 | Payload |
| --- | --- |
| `0x13` 下行 | 空，查询真实 PermitJoin |
| `0xA1` 上行 | `active:u8, remaining_seconds:u8` |
| `0x14` 下行 | `short:u16`，重新识别已知节点 |
| `0xA2` 上行，12 字节 | `IEEE[8], short:u16, state:u8, endpoint:u8` |
| `0xA2` 上行，16+2×簇数 | 上述前缀 + `profile:u16, inputCount:u8, outputCount:u8, inputClusters:u16[], outputClusters:u16[]` |
| `0xA3` 上行 | `IEEE[8], short:u16, protocol:u16, capabilities:u16` |

state：0 未知，1 识别中，2 标准设备，3 HY 就绪，4 识别失败。后端校验完整长度、IEEE 对应的当前短地址，不把旧地址的迟到信息写给新设备。
识别内存有界：每设备最多 16 端点，每端点最多 32 输入/32 输出簇；超出上限明确失败而非假装识别完整。每步最多 3 次请求、4 秒间隔；睡眠设备可唤醒后手动重试。

## 第三方协调器 / Zigbee2MQTT

“通用协调器能读”指标准 ZDO 描述符和标准 ZCL 属性可读，**不等于所有平台能自动理解磁力/麦克风/波形私有语义**。完整批次上传仍需协议适配。

`integrations/zigbee2mqtt/hy-sensor-v3.mjs` 提供只读外部转换器示例：标准温度 + FC10 版本/能力，并注册 FC10/FC11 属性名称。它故意不发送 HELLO，不开启 HY 采集、不接管波形上传；不会让节点误以为它有完整采集服务器。该转换器需要在用户实际 Zigbee2MQTT 版本及硬件上验证。

参考官方说明：[支持新设备](https://www.zigbee2mqtt.io/advanced/support-new-devices/01_support_new_devices.html)、[直接读取属性](https://www.zigbee2mqtt.io/guide/usage/mqtt_topics_and_messages.html#directly-reading-writing-zcl-attributes)。使用现代扩展 `deviceAddCustomCluster` 和 `temperature`，不宣称无需转换器即可展示全部功能。

## 现场验收（尚需硬件验证）

1. 同时更新双模节点 CPU1、TI 协调器和网关前后端；保留匹配的 BLE+Zigbee CPU2 镜像。**不要只刷一端后用旧私有协议联调。**
2. 开网→节点入网→列表自动出现→Cluster 显示→采集协议就绪→手动打开在线监测→验证原 MQTT collection/waveform。
3. 识别途中停止允许加入、等待自然到期、刷新前端、重启后端：设备都应保留，采集不应被关网中断。
4. 双节点同时加入、睡眠/掉电/改短地址、识别超时后重试、删除后重新手动登记。
5. BLE 连接期间暂停 Zigbee，断开后恢复；Flash 波形批次保留，不能占用 BLE 传感器或丢失未确认批次。
6. 将测试节点加入第三方网络（按现有明确重置/离网流程切网）：读取 Basic、温度、FC10，验证温度报告。不要同时把同一节点当作两个 Zigbee 网络成员。

本地回归入口：`tests/run-join-cluster-tests.ps1`；前端 `tests/node-interview.mjs`。编译通过和模拟测试不替代上述射频、低功耗、重启及 MQTT 实机验收。

## 本轮验证结果（2026-10-22）

- 双模节点：STM32CubeIDE 配套 GCC，Debug / Release 均编译、链接通过；修正 Release 未配置传感器搜索路径时的头文件引用。未修改 BLE GATT 源码、CPU2 镜像或纯 Zigbee 工程。
- TI 协调器：CCS 20.5 / TI Arm LLVM 4.0.4，编译、链接并生成 HEX 通过。
- 网关后端：使用本地 Zig 工具链完成 Linux x86_64 全量编译（含 SQLite）。这是虚拟机架构的编译检查，**不是 RK3506 部署包**；上板前仍需执行原 RK3506 构建脚本。
- 前端：13 个现有及新增测试均通过，生产构建通过。验证使用 `out` 下隔离的 Windows 依赖（Vite 8.2.2），没有重装共享目录中的 Linux `node_modules`。Oxlint 检查无错误，有 5 条 React 手动 memoization 优化警告。
- 新增入网/Cluster 回归：自动登记、关网不删除、状态确认与过期、动态端点、描述符/协议校验、HELLO 上行确认、识别超时/重试、数据库重启持久化、短地址复用均通过。
- 原有回归：report、monitor-switch（指定双模节点）、vibration、upload-queue、MQTT store/config/client/management 均通过。MQTT 使用本地测试服务器，**未连接现场 EMQX**。
- Zigbee2MQTT 转换器仅做 JavaScript 语法检查，尚未在实际 Zigbee2MQTT 运行环境验证。
- 删除了旧待授权面板及其专属测试文件；可从修改前 Git 标签恢复，没有删除现场设备数据。

生成产物位于双模节点工程 `Debug/STM32WB55RGV6-BLE-Zigbee.elf`、`Release/STM32WB55RGV6-BLE-Zigbee.elf`，协调器工程 `default/zc_cc1352R_coord.hex`；网关编译检查产物为 `out/gateway-server-cluster-linux`。本轮未执行任何烧录。

2026-10-23 型号重复查询修正：保留节点补发标准入网宣告的机制；去除后端重连查询，接收端按真实型号变化更新。节点待机期间的配置查询日志改为 `standby configuration query started`，采集后查询仍标记 `post-upload`，两者使用原有查询时序。
本次型号去重/自定义名称与入网回归通过，Linux x86_64 后端及双模节点 Debug/Release 编译通过。消除重复名称更新只需重新编译并重启网关后端；节点固件更新用于显示修正后的配置查询日志，协调器和前端本次无需更新。尚未对本次修正进行实机复测。

## 2026-10-23 断电恢复宣告顺序修正

双模节点恢复本地网络参数后，先通过标准 ZDO Node Descriptor 请求验证协调器可达，再统一发送一次应用主动 Device Announce，等待 HELLO 后查询配置并恢复监测。原来“恢复完成先宣告、验证成功后再宣告”的两次应用主动发送已合并；提前到达的 HELLO 不允许越过验证或主动宣告。

- 验证期间暂缓配置查询及采集；验证失败保留原网络参数与待发送宣告，沿用恢复重试定时器。
- 验证成功但 BLE 正占用无线时，保留待宣告状态，恢复 Zigbee 后继续；重复任务调度不会再次主动发送。
- 恢复验证始终采用 ZDO，即使已收到早到的私有 HELLO，也不依赖尚在识别中的私有采集通路。
- CPU2 协议栈可能自行发出标准入网/重入通知；本次保证的是应用主动宣告只发送一次，不屏蔽协议栈实际诊断消息。
- 该修正只需更新双模节点 CPU1 固件；无需为此更新前端、后端、协调器或 CPU2 镜像。

`tests/run-node-recovery-tests.ps1` 将实际节点源码中的恢复相关函数编译到模拟协议栈/调度器中，覆盖验证→单次主动宣告→配置顺序、早到 HELLO、重复调度、BLE 暂停、提交失败后重试、恢复尚未完成及新入网路径。该测试与监测开关/待机回归通过，节点 Debug/Release 编译通过；尚未执行硬件烧录或射频复测。

实机复测：完成一次采集后断电上电，依次观察 `restored coordinator verified` → 一条 `self announcement submitted` → `sampling configuration accepted` → 新批次采集。另测协调器暂时断电及恢复验证期间连接/断开 BLE，确认旧网络保留且能继续恢复。
