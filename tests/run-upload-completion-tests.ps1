param([string]$NodeRoot=(Join-Path $PSScriptRoot '../../315-research'))
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$build=Join-Path $root 'out/upload-completion-tests'
New-Item -ItemType Directory -Force $build | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR=Join-Path $root 'out/zig-cache'
$env:ZIG_LOCAL_CACHE_DIR=Join-Path $root 'out/zig-local'
if((Get-FileHash "$root/gateway-core/include/upload_completion.h").Hash -ne
   (Get-FileHash "$NodeRoot/Core/Inc/upload_completion.h").Hash){throw 'Completion protocol headers differ'}
$source=Get-Content -LiteralPath "$NodeRoot/STM32_WPAN/App/app_zigbee.c" -Raw -Encoding UTF8
$test=(Get-Content -LiteralPath "$PSScriptRoot/test_upload_completion.c" -Raw -Encoding UTF8).Replace('DB_IMPLEMENTATION',"$root/gateway-core/src/db_manager.c")
foreach($name in @('ZbApp_ClearDeferredUpload','ZbApp_AcceptCompletedBatch','ZbApp_ReleaseConfirmedBatch','ZbApp_UploadConfirm')) {
    $body=[regex]::Match($source,'(?ms)^static (?:void|int) '+$name+'\([^;{]*\)\r?\n\{.*?^\}').Value
    if(!$body){throw "Missing actual node $name"};$test+="`n$body`n"
}
$path=Join-Path $build 'completion.c'
[IO.File]::WriteAllText($path,$test,[Text.UTF8Encoding]::new($false))
$zig="$root/.codex-build-deps/ziglang/zig.exe"
& $zig cc -std=c11 -O1 -Wall -Wextra -Werror -Wno-unused-function -UNDEBUG -DUPLOAD_RESTART_GUARD_MS=35000 "-I$root/gateway-core/include" "-I$NodeRoot/Core/Inc" "-I$root/out/sqlite/sqlite-amalgamation-3500400" $path "$root/out/vibration-tests/sqlite3.o" -o "$build/completion.exe"
if($LASTEXITCODE){throw 'Completion test compilation failed'}
$run=Join-Path $build ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $run | Out-Null
Push-Location $run
try { & "$build/completion.exe";if($LASTEXITCODE){throw 'Completion replay failed'} } finally {Pop-Location}
