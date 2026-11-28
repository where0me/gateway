# 团队使用方式

仓库：`https://github.com/where0me/gateway`。分支 `main` 保存已合并的配套版本，开发在个人功能分支进行。

## 首次获取

在 GitHub Desktop 中选择 File → Clone repository，使用仓库地址，保存到 `C:\Git\gateway` 或其他本地目录。前端/后端在此副本开发；协调器按其 README 导入 CCS。节点另行克隆 `315-research`，需要跨设备测试时与本仓库并列放置。

## 每次修改

1. Fetch/Pull 更新 main，再建立功能分支。
2. 修改代码及对应说明；若更改线上协议，同时核对后端、协调器、节点的字段定义和兼容性。
3. 运行与改动相关的测试，在 Changes 中检查文件，Commit 到个人分支，再 Push/Publish branch。
4. 创建 Pull Request，确认 base 为 main、compare 为个人分支。评审/测试通过后合并，其他成员拉取 main。

GitHub 上 Pending Invite 表示尚未接受邀请；对方接受后才能作为协作者推送。

## 工作副本与原目录

本次导入没有移动原 IDE/运行目录。在原目录继续修改时，GitHub Desktop 中的仓库不会自动产生变化，需将经过测试的改动同步到本仓库。团队日常开发宜直接使用克隆的仓库目录，避免两份代码混淆。

数据库、采集数据、日志、编译产物、node_modules 和运行配置不提交；示例配置不存真实 MQTT 凭据。原有源码中的第三方版权声明保持不变。
