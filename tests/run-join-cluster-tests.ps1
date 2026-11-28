param([string]$CoordinatorRoot=(Join-Path $PSScriptRoot '../coordinator'))
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$build=Join-Path $root 'out/vibration-tests'
New-Item -ItemType Directory -Force $build | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR=Join-Path $root 'out/zig-cache'
$env:ZIG_LOCAL_CACHE_DIR=Join-Path $root 'out/zig-local'
$zig=Join-Path $root '.codex-build-deps/ziglang/zig.exe'
$sql=Join-Path $root 'out/sqlite/sqlite-amalgamation-3500400'
if(-not(Test-Path "$build/sqlite3.o")) {
    & $zig cc -O1 -c "$sql/sqlite3.c" -o "$build/sqlite3.o"
    if($LASTEXITCODE){throw 'SQLite compilation failed'}
}
& $zig cc -std=c11 -O1 -UNDEBUG "-I$root/gateway-core/include" "-I$sql" "$PSScriptRoot/test_gateway_join.c" "$root/gateway-core/src/cJSON.c" "$build/sqlite3.o" -o "$build/gateway-join.exe"
if($LASTEXITCODE){throw 'Gateway join test compilation failed'}
& $zig cc -std=c11 -Wall -Wextra -O1 -UNDEBUG "-I$CoordinatorRoot/Application" "$PSScriptRoot/test_cluster_interview.c" -o "$build/cluster-interview.exe"
if($LASTEXITCODE){throw 'Coordinator interview test compilation failed'}
Push-Location $root
try {
    & "$build/gateway-join.exe"; if($LASTEXITCODE){throw 'Gateway join test failed'}
    & "$build/cluster-interview.exe"; if($LASTEXITCODE){throw 'Coordinator interview test failed'}
    node gateway-ui/tests/node-interview.mjs; if($LASTEXITCODE){throw 'UI interview test failed'}
} finally {Pop-Location}
