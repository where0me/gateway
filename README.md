# gateway

网关前端、Linux 后端与 TI CC1352R 协调器的配套源码仓库。STM32 节点代码在独立的 `yang1768707-niko/315-research` 仓库。

## 目录

| 目录 | 内容 |
| --- | --- |
| `gateway-core/` | C 后端、串口协议、SQLite、采集入库、上传与 MQTT/Sparkplug |
| `gateway-ui/` | React/Vite 前端、设备管理、配置及数据展示 |
| `coordinator/` | CCS 工程 `zc_cc1352R_coord`，TI CC1352R1 / Z-Stack |
| `tests/` | 协议、后端和跨设备主机回归测试 |
| `docs/` | 功能、协议、部署和历史修改记录 |
| `deploy/rk3506/` | RK3506 构建/部署脚本与无凭据配置模板 |
| `integrations/` | Sparkplug、Zigbee2MQTT 等集成工具 |
| `tools/` | 本地测试依赖准备工具 |

[本次初始化范围与来源](docs/REPOSITORY_SETUP_20261128.md) · [协作流程](docs/TEAM_WORKFLOW.md) · [协调器构建](coordinator/README.md)

## Linux 后端

需要 C 编译器、make、SQLite3 开发库和 POSIX 线程。以 Debian/Ubuntu 开发环境为例：

```sh
sudo apt install build-essential libsqlite3-dev
cd gateway-core
make
cp gateway_config.example.json gateway_config.json
# 按实际连接修改 serial_path，协调器串口波特率为 921600。
./gateway-server
```

配置、数据库和日志均在本机生成，不提交到仓库。MQTT 在示例配置中关闭，账号密码为空；按实际环境配置。后端 HTTP 端口默认 8080。

## 前端

本次验证使用 Node.js 24 和 npm 11；依赖版本固定在 `gateway-ui/package-lock.json`。

```sh
cd gateway-ui
npm ci
cp .env.example .env
npm run dev
```

默认开发代理目标在 `.env.example` 中设为 `http://127.0.0.1:8080`。注意：当前部分 API/WebSocket 直接访问浏览器所在主机名的 8080 端口；异机联调还需核对这些直连请求，仅修改 `.env` 不能覆盖全部请求。生产构建用 `npm run build`；RK3506 部署见 [部署说明](deploy/rk3506/README.md)。

## 协调器和节点

在 CCS 中导入 `coordinator/` 工程，使用 SimpleLink CC13xx/CC26xx SDK **8.32.00.07**，并让 CCS 重新生成 SysConfig 和 makefile。不要直接使用旧路径的生成 makefile。

原始源码、IDE 配置和 SysConfig 基线纳入仓库；编译产物、设备状态和原工程的 `.git` 不纳入。第三方 SDK 文件保留原版权声明，SDK/工具链仍需另行安装。

跨节点回归默认寻找并列目录 `../315-research`。如果节点仓库放在别处，在对应脚本传入 `-NodeRoot` 或 `-FusionNodeRoot`。

## 测试

前端构建：`cd gateway-ui && npm ci && npm run build`。

Linux 后端测试：在 `gateway-core/` 中运行 `make test`；它需要开发环境的 SQLite 和线程库，部分测试可能创建临时数据，应在开发副本运行。

Windows 跨设备主机测试使用 Zig 0.14.1 和 SQLite 3.50.4 的 amalgamation 源码。先按 [依赖准备说明](docs/HOST_TESTS.md) 准备，再从仓库根目录执行：

```powershell
./tests/run-serial-comm-test.ps1
./tests/run-manual-admission-tests.ps1
```

这些测试不替代真实无线通信、串口接线、功耗和板上验证。`docs/` 中带日期的文件是历史设计记录；当前行为以配套源码和最新说明为准。
