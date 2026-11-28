# Windows 主机回归依赖

现有跨设备脚本使用 Zig 0.14.1 和 SQLite 3.50.4 源码，编译本机模拟程序。依赖不进入 Git。

准备一个完整 Zig 目录（包含 `zig.exe` 和 `lib/`）以及 SQLite amalgamation 目录（包含 `sqlite3.c/sqlite3.h`），然后：

```powershell
./tools/Prepare-HostTests.ps1 -ZigDirectory '<完整Zig目录>' -SqliteDirectory '<SQLite amalgamation目录>'
./tests/run-serial-comm-test.ps1
./tests/run-manual-admission-tests.ps1
```

准备脚本仅向忽略目录 `.codex-build-deps`、`out` 复制依赖并编译 SQLite；不会下载内容，也不操作设备。节点仓库默认在本仓库的并列目录 `315-research`。各测试仍保留参数，可通过 `-NodeRoot`、`-CoordinatorRoot` 等指定其他位置。历史专项测试所需额外输入请查看对应脚本。
