$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'out/capture-time-tests'
New-Item -ItemType Directory -Force $build | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $root 'out/zig-cache'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $root 'out/zig-local'
$zig = Join-Path $root '.codex-build-deps/ziglang/zig.exe'
$sql = Join-Path $root 'out/sqlite/sqlite-amalgamation-3500400'
$exe = Join-Path $build 'capture-time.exe'
& $zig cc -std=gnu11 -O1 -Wall -Wextra -Werror -UNDEBUG "-I$root/gateway-core/include" "-I$sql" `
    "$PSScriptRoot/test_capture_time.c" "$root/gateway-core/src/cJSON.c" "$root/out/vibration-tests/sqlite3.o" -o $exe
if ($LASTEXITCODE) { throw 'Capture-time compilation failed' }
Push-Location $build
try {
    & $exe
    if ($LASTEXITCODE) { throw 'Capture-time tests failed' }
} finally { Pop-Location }
