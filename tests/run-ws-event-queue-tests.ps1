$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$build=Join-Path $root 'out/ws-event-queue-tests'
New-Item -ItemType Directory -Force $build | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR=Join-Path $root 'out/zig-cache';$env:ZIG_LOCAL_CACHE_DIR=Join-Path $root 'out/zig-local'
$source=Get-Content -Raw -Encoding UTF8 "$root/gateway-core/src/main.c"
$test=Get-Content -Raw -Encoding UTF8 "$PSScriptRoot/test_ws_event_queue.c"
foreach($name in @('queue_ws_event_locked','task_broadcast_ws')) {
    $body=[regex]::Match($source,'(?ms)^(?:static )?void '+$name+'\([^;{]*\)\s*\r?\n\{.*?^\}').Value
    if(!$body){throw "Cannot extract $name"};$test+="`n$body`n"
}
$file=Join-Path $build 'queue.c';[IO.File]::WriteAllText($file,$test,[Text.UTF8Encoding]::new($false))
& "$root/.codex-build-deps/ziglang/zig.exe" cc -std=c11 -O1 -Wall -Wextra -Werror -UNDEBUG "-I$root/gateway-core/include" $file -o "$build/queue.exe"
if($LASTEXITCODE){throw 'WebSocket queue compile failed'}
& "$build/queue.exe";if($LASTEXITCODE){throw 'WebSocket queue test failed'}
