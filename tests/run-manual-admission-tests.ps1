param([string]$NodeRoot=(Join-Path $PSScriptRoot '../../315-research'),
      [string]$CoordinatorRoot=(Join-Path $PSScriptRoot '../coordinator'))
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$build=Join-Path $root ('out/manual-admission-tests/'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $build|Out-Null
$env:ZIG_GLOBAL_CACHE_DIR="$root/out/zig-cache";$env:ZIG_LOCAL_CACHE_DIR="$root/out/zig-local"
$zig="$root/.codex-build-deps/ziglang/zig.exe";$sql="$root/out/sqlite/sqlite-amalgamation-3500400"
& $zig cc -std=gnu11 -O1 -Wall -Wextra -Werror -Wno-unused-function -UNDEBUG "-I$root/gateway-core/include" "-I$sql" "$root/tests/test_manual_admission.c" "$root/gateway-core/src/cJSON.c" "$root/out/vibration-tests/sqlite3.o" -o "$build/manual.exe"
if($LASTEXITCODE){throw 'Manual admission test compilation failed'}
Push-Location $build
try {& "$build/manual.exe";if($LASTEXITCODE){throw 'Manual admission test failed'}}finally{Pop-Location}
$source=Get-Content -Raw -Encoding UTF8 "$NodeRoot/STM32_WPAN/App/app_zigbee.c"
$test=Get-Content -Raw -Encoding UTF8 "$PSScriptRoot/test_node_manual_admission.c"
foreach($name in @('ZbApp_CheckAuthorizationBudget','ZbApp_ApplyMonitorSwitch')) {
    $body=[regex]::Match($source,'(?ms)^static (?:void|uint8_t) '+$name+'\([^;{]*\)\r?\n\{.*?^\}').Value
    if(!$body){throw "Missing $name"};$test+="`n$body`n"
}
$body=[regex]::Match($source,'(?ms)^static void ZbApp_ScheduleMonitorStandby\(void\)\r?\n\{.*?^\}').Value
$end=$body.IndexOf('  if(ZbUploadQueueSleeping')
if($end -lt 0){throw 'Cannot locate standby queue branch'}
$prefix=$body.Substring($body.IndexOf('{')+1,$end-$body.IndexOf('{')-1)
$test+="`nstatic void pending_standby(void) {`n$prefix`n}`n"
[IO.File]::WriteAllText("$build/node.c",$test,[Text.UTF8Encoding]::new($false))
& $zig cc -std=c11 -O1 -Wall -Wextra -Werror -UNDEBUG "-I$NodeRoot/Core/Inc" "$build/node.c" -o "$build/node.exe"
if($LASTEXITCODE){throw 'Node manual admission compilation failed'}
& "$build/node.exe";if($LASTEXITCODE){throw 'Node manual admission failed'}
$source=Get-Content -Raw -Encoding UTF8 "$CoordinatorRoot/Application/gateway_bridge.c"
$test=Get-Content -Raw -Encoding UTF8 "$PSScriptRoot/test_continuous_permit.c"
foreach($name in @('permitWanted','permitTick','permitState')) {
    $body=[regex]::Match($source,'(?ms)^static (?:void|uint8_t) '+$name+'\([^;{]*\)\r?\n\{.*?^\}').Value
    if(!$body){throw "Missing $name"};$test+="`n$body`n"
}
[IO.File]::WriteAllText("$build/permit.c",$test,[Text.UTF8Encoding]::new($false))
& $zig cc -std=c11 -O1 -Wall -Wextra -Werror -UNDEBUG "$build/permit.c" -o "$build/permit.exe"
if($LASTEXITCODE){throw 'Continuous permit compilation failed'}
& "$build/permit.exe";if($LASTEXITCODE){throw 'Continuous permit failed'}
