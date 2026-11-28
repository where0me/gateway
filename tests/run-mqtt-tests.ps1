param([switch]$RealEmqx)
$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot -Parent
$zigExe=Join-Path $projectRoot '.codex-build-deps/ziglang/zig.exe'
$buildDir=Join-Path $projectRoot 'out/mqtt-tests'
New-Item -ItemType Directory -Force $buildDir | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR=Join-Path $projectRoot 'out/zig-cache'
$env:ZIG_LOCAL_CACHE_DIR=Join-Path $projectRoot 'out/zig-local'
$sqliteDir=Join-Path $projectRoot 'out/sqlite/sqlite-amalgamation-3500400'
$sqliteObject=Join-Path $projectRoot 'out/vibration-tests/sqlite3.o'
if(-not (Test-Path -LiteralPath $sqliteObject)) {
    & $zigExe cc -O1 -c (Join-Path $sqliteDir 'sqlite3.c') -o $sqliteObject
    if($LASTEXITCODE -ne 0){throw 'SQLite dependency compilation failed'}
}
foreach($test in @('mqtt_store','mqtt_backlog','mqtt_config','mqtt_client','mqtt_management')) {
    $testExe=Join-Path $buildDir "$test.exe"
    $testArgs=@('cc','-std=c11','-O1','-g','-UNDEBUG',"-I$projectRoot/gateway-core/include", "-I$sqliteDir",
        (Join-Path $PSScriptRoot "test_$test.c"),"$projectRoot/gateway-core/src/mqtt_store.c",
        "$projectRoot/gateway-core/src/cJSON.c",$sqliteObject)
    if($test -notin @('mqtt_store','mqtt_backlog')) {
        $testArgs+=@("$projectRoot/gateway-core/src/gateway_mqtt.c","$projectRoot/gateway-core/src/gateway_sparkplug.c","$projectRoot/gateway-core/src/sparkplug_b.c","$projectRoot/gateway-core/src/mongoose.c",'-lws2_32','-liphlpapi')
    }
    if($test -eq 'mqtt_management') { $testArgs+="$projectRoot/gateway-core/src/gateway_mqtt_api.c" }
    & $zigExe @testArgs -o $testExe
    if($LASTEXITCODE -ne 0){throw "Compile failed: $test"}
    if($test -eq 'mqtt_store'){& $testExe (Join-Path $buildDir 'restart-test.db')}
    elseif($test -eq 'mqtt_backlog'){& $testExe}
    elseif($test -eq 'mqtt_config'){& $testExe (Join-Path $buildDir 'config-test.json')}
    elseif($test -eq 'mqtt_management'){& $testExe (Join-Path $buildDir 'management-test.json')}
    else {& $testExe}
    if($LASTEXITCODE -ne 0){throw "Test failed: $test"}
    if($test -eq 'mqtt_client' -and $RealEmqx) {
        & $testExe '--real-emqx' (Join-Path $projectRoot 'gateway-core/gateway_config.json')
        if($LASTEXITCODE -ne 0){throw 'Real EMQX isolated-topic test failed'}
    }
}
