$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'out/announcement-log-tests'
New-Item -ItemType Directory -Force $build | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $root 'out/zig-cache'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $root 'out/zig-local'
$source = Get-Content -LiteralPath "$root/gateway-core/src/main.c" -Raw -Encoding UTF8
$handler = [regex]::Matches($source, '(?ms)^void handle_cmd_84_device_join\([^;{]*\)\s*\r?\n\{.*?^\}')
if ($handler.Count -ne 1) { throw 'Cannot extract announcement handler' }
$harness = Get-Content -LiteralPath "$PSScriptRoot/test_announcement_log.c" -Raw -Encoding UTF8
$harness = $harness.Replace('../gateway-core/src/db_manager.c', "$root/gateway-core/src/db_manager.c")
$test = Join-Path $build 'announcement-log.c'
[IO.File]::WriteAllText($test, $harness + "`n" + $handler[0].Value, [Text.UTF8Encoding]::new($false))
$exe = Join-Path $build 'announcement-log.exe'
& "$root/.codex-build-deps/ziglang/zig.exe" cc -std=gnu11 -O1 -Wall -Wextra -Werror -UNDEBUG `
    "-I$root/gateway-core/include" "-I$root/out/sqlite/sqlite-amalgamation-3500400" `
    $test "$root/gateway-core/src/cJSON.c" "$root/out/vibration-tests/sqlite3.o" -o $exe
if ($LASTEXITCODE) { throw 'Announcement-log compilation failed' }
& $exe
if ($LASTEXITCODE) { throw 'Announcement-log test failed' }
