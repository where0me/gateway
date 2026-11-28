param([string]$NodeRoot=(Join-Path $PSScriptRoot '../../315-research'),
      [string]$CoordinatorRoot=(Join-Path $PSScriptRoot '../coordinator'))
$ErrorActionPreference='Stop'
$OutputEncoding=[Text.UTF8Encoding]::new($false)
$root=Split-Path $PSScriptRoot -Parent
$build=Join-Path $root 'out/scalar-delivery-tests'
New-Item -ItemType Directory -Force $build | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR=Join-Path $root 'out/zig-cache'
$env:ZIG_LOCAL_CACHE_DIR=Join-Path $root 'out/zig-local'
$reference=Get-Content -Raw -Encoding UTF8 "$root/gateway-core/include/scalar_delivery.h"
foreach($path in @("$NodeRoot/Core/Inc/scalar_delivery.h","$CoordinatorRoot/Application/scalar_delivery.h")) {
    if((Get-Content -Raw -Encoding UTF8 $path).Replace("`r",'') -cne $reference.Replace("`r",'')){throw "Scalar protocol mismatch: $path"}
}
$source=Get-Content -Raw -Encoding UTF8 "$NodeRoot/STM32_WPAN/App/app_zigbee.c"
$typeSource=Get-Content -Raw -Encoding UTF8 "$NodeRoot/STM32_WPAN/App/custom_app.h"
$snapshot=[regex]::Match($typeSource,'(?s)typedef struct\s*\{\s*int16_t temperature_centi_c;.*?\} Custom_APP_ZigbeeSnapshot_t;').Value
if(-not $snapshot){throw 'Cannot extract actual Flash snapshot type'}
$test=(Get-Content -Raw -Encoding UTF8 "$PSScriptRoot/test_scalar_delivery.c").Replace('DB_IMPLEMENTATION',"$root/gateway-core/src/db_manager.c").Replace('/* SNAPSHOT_TYPE */',$snapshot)
foreach($name in @('ZbApp_Magnetic0p1uTToRaw','ZbApp_ScalarPacket','ZbApp_PrepareScalarDelivery','ZbApp_ScalarSendItem','ZbApp_ScalarSendManifest','ZbApp_SendSnapshot')) {
    $found=[regex]::Matches($source,'(?ms)^static (?:int16_t|unsigned|int|void) '+$name+'\([^;{]*\)\r?\n\{.*?^\}')
    if($found.Count -ne 1){throw "Cannot extract $name"}
    $test+="`n"+$found[0].Value+"`n"
}
$backend=Get-Content -Raw -Encoding UTF8 "$root/gateway-core/src/main.c"
$logger=[regex]::Match($backend,'(?ms)^static void log_scalar_capture_missing\([^;{]*\)\r?\n\{.*?^\}').Value
if(-not $logger){throw 'Cannot extract actual capture warning logger'}
$test+="`n"+$logger+"`n"
$path=Join-Path $build 'scalar-delivery.c'
[IO.File]::WriteAllText($path,$test,[Text.UTF8Encoding]::new($false))
$zig=Join-Path $root '.codex-build-deps/ziglang/zig.exe'
& $zig cc -std=c11 -O1 -Wall -Wextra -UNDEBUG "-I$NodeRoot/Core/Inc" "-I$root/gateway-core/include" `
    "-I$root/out/sqlite/sqlite-amalgamation-3500400" $path "$root/gateway-core/src/mqtt_store.c" `
    "$root/gateway-core/src/cJSON.c" "$root/gateway-core/src/gateway_log.c" "$root/out/vibration-tests/sqlite3.o" -lpthread -o "$build/scalar-delivery.exe"
if($LASTEXITCODE){throw 'Scalar delivery compilation failed'}
& "$build/scalar-delivery.exe"
if($LASTEXITCODE){throw 'Scalar delivery test failed'}
& $zig cc -std=c11 -O1 -UNDEBUG -DTEST_RESTART_LEASE -DUPLOAD_RESTART_GUARD_MS=35000 "-I$NodeRoot/Core/Inc" "-I$root/gateway-core/include" `
    "-I$root/out/sqlite/sqlite-amalgamation-3500400" $path "$root/gateway-core/src/mqtt_store.c" `
    "$root/gateway-core/src/cJSON.c" "$root/gateway-core/src/gateway_log.c" "$root/out/vibration-tests/sqlite3.o" -lpthread -o "$build/gateway-lease.exe"
if($LASTEXITCODE){throw 'Gateway lease compilation failed'}
& "$build/gateway-lease.exe"
if($LASTEXITCODE){throw 'Gateway lease test failed'}
