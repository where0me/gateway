$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$build=Join-Path $root 'out/sparkplug-tests'
New-Item -ItemType Directory -Force $build | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR=Join-Path $root 'out/zig-cache'
$env:ZIG_LOCAL_CACHE_DIR=Join-Path $root 'out/zig-local'
$sources=@('gateway_mqtt','gateway_sparkplug','sparkplug_b','mqtt_store','mongoose','cJSON') | ForEach-Object {Join-Path $root "gateway-core/src/$_.c"}
& "$root/.codex-build-deps/ziglang/zig.exe" cc -std=c11 -O1 -g -UNDEBUG -Wall -Wextra "-I$root/gateway-core/include" "-I$root/out/sqlite/sqlite-amalgamation-3500400" "$root/tests/test_sparkplug.c" @sources "$root/out/vibration-tests/sqlite3.o" -lws2_32 -liphlpapi -o "$build/sparkplug.exe"
if($LASTEXITCODE){throw 'Sparkplug compilation failed'}
& "$build/sparkplug.exe" $build
if($LASTEXITCODE){throw 'Sparkplug integration failed'}
