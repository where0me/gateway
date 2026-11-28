# 仓库首次导入（2026-11-28）

远程仓库是已有的 `where0me/gateway`，原 main 提交为 `5846bb6`（Initial commit，仅 README）。本次在本地 `codex/gateway-initial-import` 分支整理源码，不自动提交或推送。

## 来源与范围

- 网关：`C:\Users\Administrator\Documents\Codex\Zigbee-Gateway` 的当前工作副本，包含尚未提交到其旧本地 Git 的最新修改。
- 协调器：`C:\Users\Administrator\workspace_ccstheia\zc_cc1352R_coord` 的当前工作副本，放入 `coordinator/`。
- 节点仍独立保存在 `315-research` 仓库。

后端/前端/协调器的业务源码保持原样；适配内容仅为本仓库目录结构、测试脚本默认路径、授权测试中适配最新 PendingLink 接口的模拟代码、去除凭据的示例配置、README 和忽略规则。

## 纳入内容

前后端源码、依赖清单/锁文件、协调器源码/IDE配置/SysConfig基线、测试、部署脚本和技术说明。原项目 `.git` 不嵌套复制，不导入旧工作区的 Git 历史；它们仍保存在原目录。

## 排除内容

数据库、WAL/SHM、设备采集数据、运行日志、真实运行配置、node_modules、构建结果、旧压缩包、本地生成的图表和 Office 工作报告（前端源码自带的静态资源保留），以及 Codex/IDE 工作区缓存。

本次原始源码存档和逐文件 SHA256 清单放在原网关工作目录 `output/网关仓库初始化_20261128_*`，不进入待上传仓库。原有运行数据和旧工程保持原状。

## 发布步骤

在 GitHub Desktop 使用 File → Add local repository 添加 `C:\Git\gateway`。在本地导入分支检查并 Commit，再 Publish branch，创建合入 main 的 Pull Request。不要重新创建同名远程仓库。

## 本次整理验证

- 新目录下 `npm ci --ignore-scripts` 和 `npm run build` 通过。
- Linux x86_64-musl 后端交叉编译通过，生成 ELF；未在 Windows 上运行该 Linux 程序。
- 新目录下通信测试、待授权链路自检、串口命令测试及网关诊断回显通过。
- 后端手动授权、节点待授权/授权切换及协调器连续允许加入模拟测试通过。
- 协调器/后端 6 份同名共享协议头内容一致，工程 XML 可解析，运行数据/依赖/编译产物忽略规则通过。
- 本次未执行 CCS 的完整协调器构建或烧录，也未重新测试实机链路。业务源码与导入时工作副本一致。
