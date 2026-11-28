param([string]$NodeRoot=(Join-Path $PSScriptRoot '../../315-research'))
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$build=Join-Path $root 'out/upload-scheduler-tests'
New-Item -ItemType Directory -Force $build | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR=Join-Path $root 'out/zig-cache'
$env:ZIG_LOCAL_CACHE_DIR=Join-Path $root 'out/zig-local'
$zig=Join-Path $root '.codex-build-deps/ziglang/zig.exe'
$sql=Join-Path $root 'out/sqlite/sqlite-amalgamation-3500400'
& $zig cc -O1 -c "$sql/sqlite3.c" -o "$build/sqlite3.o"
if($LASTEXITCODE){throw 'SQLite build failed'}
foreach($name in @('monitor_switch','node_groups')) {
    & $zig cc -std=c11 "-I$root/gateway-core/include" "-I$sql" "-I$NodeRoot/Core/Inc" "$PSScriptRoot/test_$name.c" "$build/sqlite3.o" -o "$build/$name.exe"
    if($LASTEXITCODE){throw 'Test build failed'}
    Push-Location $root
    try { & "$build/$name.exe"; if($LASTEXITCODE){throw 'Test failed'} }
    finally { Pop-Location }
}
