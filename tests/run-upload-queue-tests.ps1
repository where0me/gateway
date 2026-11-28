param([string]$NodeRoot=(Join-Path $PSScriptRoot '../../315-research'))
$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot -Parent
$zigExe=Join-Path $projectRoot '.codex-build-deps/ziglang/zig.exe'
$buildDir=Join-Path $projectRoot 'out/vibration-tests'
New-Item -ItemType Directory -Force $buildDir | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR=Join-Path $projectRoot 'out/zig-cache'
$env:ZIG_LOCAL_CACHE_DIR=Join-Path $projectRoot 'out/zig-local'
$sqliteDir=Join-Path $projectRoot 'out/sqlite/sqlite-amalgamation-3500400'
$sqliteObject=Join-Path $buildDir 'sqlite3.o'
if(-not (Test-Path -LiteralPath $sqliteObject)) {
    & $zigExe cc -O1 -c (Join-Path $sqliteDir 'sqlite3.c') -o $sqliteObject
    if($LASTEXITCODE -ne 0){throw 'SQLite test dependency compilation failed'}
}
foreach($test in @('upload_admission','node_wake_queue','upload_queue_gateway')) {
    $testExe=Join-Path $buildDir "$test.exe"
    $testArgs=@('cc','-std=c11','-O1','-UNDEBUG',"-I$projectRoot/gateway-core/include", "-I$NodeRoot/Core/Inc",
        "-I$projectRoot/out/sqlite/sqlite-amalgamation-3500400",(Join-Path $PSScriptRoot "test_$test.c"))
    if($test -eq 'upload_queue_gateway'){$testArgs+=$sqliteObject}
    & $zigExe @testArgs -o $testExe
    if($LASTEXITCODE -ne 0){throw "Compile failed: $test"}
    & $testExe
    if($LASTEXITCODE -ne 0){throw "Test failed: $test"}
}
