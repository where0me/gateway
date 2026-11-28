param([string]$NodeRoot=(Join-Path $PSScriptRoot '../../315-research'),
      [string]$CoordinatorRoot=(Join-Path $PSScriptRoot '../coordinator'))
$ErrorActionPreference='Stop'
$OutputEncoding=[Text.UTF8Encoding]::new($false)
$root=Split-Path $PSScriptRoot -Parent
$build=Join-Path $root 'out/control-push-tests'
New-Item -ItemType Directory -Force $build | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR=Join-Path $root 'out/zig-cache'
$env:ZIG_LOCAL_CACHE_DIR=Join-Path $root 'out/zig-local'
$zig=Join-Path $root '.codex-build-deps/ziglang/zig.exe'
$sql=Join-Path $root 'out/sqlite/sqlite-amalgamation-3500400'
$reference=Get-Content -Raw -Encoding UTF8 "$root/gateway-core/include/control_sync.h"
foreach($copy in @("$NodeRoot/Core/Inc/control_sync.h","$CoordinatorRoot/Application/control_sync.h")) {
    if((Get-Content -Raw -Encoding UTF8 $copy).Replace("`r",'') -cne $reference.Replace("`r",'')) {throw "Wire definition differs: $copy"}
}
$source=Get-Content -Raw -Encoding UTF8 "$root/gateway-core/src/main.c"
$test=Get-Content -Raw -Encoding UTF8 "$PSScriptRoot/test_control_push.c"
$test=$test.Replace('../gateway-core/src/db_manager.c',"$root/gateway-core/src/db_manager.c")
$join=Get-Content -Raw -Encoding UTF8 "$root/gateway-core/include/gateway_join.h"
$confirm=[regex]::Match($join,'(?ms)^static void confirm_visible_node\([^;{]*\)\r?\n\{.*?^\}').Value
if(-not $confirm){throw 'Cannot extract join confirmation'}
$test+="`n$confirm`n"
foreach($name in @('lookup_mac_by_short','build_monitor_config_payload','control_delivery','send_monitor_config_scoped',
                  'send_monitor_resume_schedule','handle_control_status','handle_cmd_99_config_query','api_handle_device_monitor')) {
    $pattern='(?ms)^(?:static )?(?:void|int) '+$name+'\([^;{]*\)\r?\n\{.*?^\}'
    $found=[regex]::Matches($source,$pattern)
    if($found.Count -ne 1){throw "Cannot extract $name"}
    if($name -eq 'control_delivery') {
        $globals=[regex]::Match($source,'(?ms)^static pthread_mutex_t control_delivery_mutex.*?control_deliveries\[128\];').Value
        if(-not $globals){throw 'Missing push delivery cache'}
        $test+="`n"+$globals
    }
    $test+="`n"+$found[0].Value+"`n"
}
$path=Join-Path $build 'control-push.c'
[IO.File]::WriteAllText($path,$test,[Text.UTF8Encoding]::new($false))
& $zig cc -std=gnu11 -O1 -Wall -Wextra -UNDEBUG "-I$root/gateway-core/include" "-I$sql" `
    $path "$root/gateway-core/src/cJSON.c" "$root/out/vibration-tests/sqlite3.o" -lpthread -o "$build/control-push.exe"
if($LASTEXITCODE){throw 'Push backend test compilation failed'}
& "$build/control-push.exe"
if($LASTEXITCODE){throw 'Push backend test failed'}
