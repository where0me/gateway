param([string]$NodeRoot=(Join-Path $PSScriptRoot '../../315-research'))
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$build=Join-Path $root 'out/flash-overwrite-tests'
New-Item -ItemType Directory -Force $build | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR="$root/out/zig-cache"; $env:ZIG_LOCAL_CACHE_DIR="$root/out/zig-local"
$zig="$root/.codex-build-deps/ziglang/zig.exe"
$store=Get-Content -LiteralPath "$NodeRoot/Core/Src/zigbee_vibration_store.c" -Raw -Encoding UTF8
$fixture=Get-Content -LiteralPath "$PSScriptRoot/test_flash_overwrite.c" -Raw -Encoding UTF8
$source=$fixture.Replace('/* ACTUAL_STORE */',$store)
[IO.File]::WriteAllText("$build/flash-overwrite.c",$source,[Text.UTF8Encoding]::new($false))
& $zig cc -std=c11 -O1 -Wall -Wextra -Werror -UNDEBUG "-I$PSScriptRoot/stubs" "-I$NodeRoot/Core/Inc" `
    "$build/flash-overwrite.c" -o "$build/flash-overwrite.exe"
if ($LASTEXITCODE) { throw 'Flash overwrite compilation failed' }
& "$build/flash-overwrite.exe"
if ($LASTEXITCODE) { throw 'Flash overwrite regression failed' }
# The actual uploader must pin before use and centralize every reset/unpin.
$app=Get-Content -LiteralPath "$NodeRoot/STM32_WPAN/App/app_zigbee.c" -Raw -Encoding UTF8
if ([regex]::Matches($app,'memset\(&ZbDeferred').Count -ne 1 -or
    $app -notmatch 'ProtectBatch\(ZbDeferred.batch\)') { throw 'Upload pin/reset integration missing' }
$start=$app.IndexOf('if (!ZigbeeVibrationStore_ProtectBatch(ZbDeferred.batch))')
$active=$app.IndexOf('ZbDeferred.active=1U;')
if ($start -lt 0 -or $start -gt $active) { throw 'Pin must precede active upload context' }
foreach ($name in @('test_fusion_vibration_store','test_node_store_reset','test_async_vibration_store')) {
    & $zig cc -std=c11 -O1 -Wall -Wextra -UNDEBUG "-I$PSScriptRoot/stubs" "-I$NodeRoot/Core/Inc" `
        "$PSScriptRoot/$name.c" "$NodeRoot/Core/Src/zigbee_vibration_store.c" -o "$build/$name.exe"
    if ($LASTEXITCODE) { throw "$name compilation failed" }
    & "$build/$name.exe"
    if ($LASTEXITCODE) { throw "$name failed" }
}
Write-Output 'Upload pin integration and existing Flash/metadata/async/reset regressions PASS; hardware not accessed.'
