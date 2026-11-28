param([string]$NodeRoot=(Join-Path $PSScriptRoot '../../315-research'),
      [string]$CoordinatorRoot=(Join-Path $PSScriptRoot '../coordinator'))
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$build=Join-Path $root 'out/reliable-control-tests'
New-Item -ItemType Directory -Force $build | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR="$root/out/zig-cache";$env:ZIG_LOCAL_CACHE_DIR="$root/out/zig-local"
$zig="$root/.codex-build-deps/ziglang/zig.exe"
if((Get-FileHash "$root/gateway-core/include/upload_lease.h").Hash -ne (Get-FileHash "$NodeRoot/Core/Inc/upload_lease.h").Hash){throw 'Lease headers differ'}
if((Get-FileHash "$root/gateway-core/include/control_sync.h").Hash -ne (Get-FileHash "$CoordinatorRoot/Application/control_sync.h").Hash){throw 'Control status headers differ'}
& $zig cc -std=c11 -O1 -Wall -Wextra -Werror -UNDEBUG "-I$root/gateway-core/include" "-I$NodeRoot/Core/Inc" "-I$CoordinatorRoot/Application" "$PSScriptRoot/test_reliable_control.c" -o "$build/reliable-control.exe"
if($LASTEXITCODE){throw 'Reliable control compilation failed'}
& "$build/reliable-control.exe"
if($LASTEXITCODE){throw 'Reliable control failed'}
$test=Get-Content -Raw -Encoding UTF8 "$PSScriptRoot/test_upload_recovery.c"
$source=Get-Content -Raw -Encoding UTF8 "$NodeRoot/STM32_WPAN/App/app_zigbee.c"
foreach($name in @('ZbApp_ClearDeferredUpload','ZbApp_UploadConfirm','ZbApp_ServiceUploadRecovery',
                  'ZbApp_ResetTxQueue','ZbApp_StartLinkRecovery','ZbApp_VerifyParent','ZbApp_CommandConfirm',
                  'ZbApp_CompleteJoinFailureCycle')) {
    $body=[regex]::Match($source,'(?ms)^static (?:void|uint8_t) '+$name+'\([^;{]*\)\r?\n\{.*?^\}').Value
    if(-not $body){throw "Cannot extract $name"};$test+="`n$body`n"
}
$source=Get-Content -Raw -Encoding UTF8 "$NodeRoot/Core/Inc/sensor_zcl_impl.h"
$body=[regex]::Match($source,'(?ms)^static void ZbApp_ParentDescriptor\([^;{]*\)\r?\n\{.*?^\}').Value
if(-not $body){throw 'Cannot extract real ZDO parent callback'}
$test+="`n$body`n"
[IO.File]::WriteAllText("$build/upload-recovery.c",$test,[Text.UTF8Encoding]::new($false))
& $zig cc -std=c11 -O1 -Wall -Wextra -Werror -Wno-unused-variable -UNDEBUG "-I$NodeRoot/Core/Inc" "$build/upload-recovery.c" -o "$build/upload-recovery.exe"
if($LASTEXITCODE){throw 'Actual recovery compilation failed'}
& "$build/upload-recovery.exe"
if($LASTEXITCODE){throw 'Actual recovery failed'}
$test=Get-Content -Raw -Encoding UTF8 "$PSScriptRoot/test_coordinator_retry.c"
$source=Get-Content -Raw -Encoding UTF8 "$CoordinatorRoot/Application/gateway_bridge.c"
foreach($name in @('controlAttempt','controlApplied')) {
    $body=[regex]::Match($source,'(?ms)^static void '+$name+'\([^;{]*\)\r?\n\{.*?^\}').Value
    if(-not $body){throw "Cannot extract coordinator $name"};$test+="`n$body`n"
}
[IO.File]::WriteAllText("$build/coordinator-retry.c",$test,[Text.UTF8Encoding]::new($false))
& $zig cc -std=c11 -O1 -Wall -Wextra -Werror -UNDEBUG "-I$CoordinatorRoot/Application" "$build/coordinator-retry.c" -o "$build/coordinator-retry.exe"
if($LASTEXITCODE){throw 'Coordinator retry compilation failed'}
& "$build/coordinator-retry.exe"
if($LASTEXITCODE){throw 'Actual coordinator retry failed'}
