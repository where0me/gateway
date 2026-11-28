$ErrorActionPreference = 'Stop'
$OutputEncoding = [System.Text.UTF8Encoding]::new($false)
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'out/latest-readings-tests'
$sql = Join-Path $root 'out/sqlite/sqlite-amalgamation-3500400'
$zig = Join-Path $root '.codex-build-deps/ziglang/zig.exe'
New-Item -ItemType Directory -Force $build | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $root 'out/zig-cache'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $root 'out/zig-local'
$sqliteObject = Join-Path $root 'out/vibration-tests/sqlite3.o'
if (-not(Test-Path $sqliteObject)) {
    $sqliteObject = Join-Path $build 'sqlite3.o'
    & $zig cc -O1 -c "$sql/sqlite3.c" -o $sqliteObject
    if ($LASTEXITCODE) { throw 'SQLite compilation failed' }
}
$source = Get-Content -LiteralPath "$root/gateway-core/src/main.c" -Raw -Encoding UTF8
$api = [regex]::Matches($source, '(?ms)^void api_handle_device_list\([^;{]*\)\s*\r?\n\{.*?^\}')
if ($api.Count -ne 1) { throw 'Cannot extract device-list API' }
$harness = Get-Content -LiteralPath "$PSScriptRoot/test_latest_readings.c" -Raw -Encoding UTF8
$test = Join-Path $build 'latest-readings.c'
# Keep the relative include in the checked-in harness usable from this directory.
$harness = $harness.Replace('../gateway-core/src/db_manager.c', "$root/gateway-core/src/db_manager.c")
[IO.File]::WriteAllText($test, $harness + "`n" + $api[0].Value, [Text.UTF8Encoding]::new($false))
$exe = Join-Path $build 'latest-readings.exe'
& $zig cc -std=gnu11 -O1 -Wall -Wextra -UNDEBUG "-I$root/gateway-core/include" "-I$sql" `
    $test "$root/gateway-core/src/cJSON.c" $sqliteObject -o $exe
if ($LASTEXITCODE) { throw 'Latest-readings test compilation failed' }
& $exe
if ($LASTEXITCODE) { throw 'Latest-readings API test failed' }
