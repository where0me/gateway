$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$build=Join-Path $root 'out/upload-timing-tests'
New-Item -ItemType Directory -Force $build | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR=Join-Path $root 'out/zig-cache'
$env:ZIG_LOCAL_CACHE_DIR=Join-Path $root 'out/zig-local'
$zig=Join-Path $root '.codex-build-deps/ziglang/zig.exe'
foreach($test in @('upload_admission','upload_timing','upload_queue_gateway','upload_timing_summary')) {
    $testPath="$root/tests/test_$test.c"
    if($test -eq 'upload_timing_summary') {
        $main=Get-Content -Raw -Encoding UTF8 "$root/gateway-core/src/main.c"
        $handler=[regex]::Match($main,'(?ms)^void handle_cmd_8c_vibration_summary\([^;{]*\)\r?\n\{.*?^\}').Value
        $timing=[regex]::Match($main,'(?ms)^static void upload_timing_sample\([^;{]*\)\r?\n\{.*?^\}').Value
        if(!$handler){throw 'Cannot extract actual feature-summary handler'}
        if(!$timing){throw 'Cannot extract actual upload timing logger'}
        $fixture=(Get-Content -Raw -Encoding UTF8 $testPath).Replace('../gateway-core/src/db_manager.c',"$root/gateway-core/src/db_manager.c")
        $testPath=Join-Path $build 'actual-summary.c'
        [IO.File]::WriteAllText($testPath,$fixture+"`n"+$handler+"`n"+$timing,[Text.UTF8Encoding]::new($false))
    }
    $compilerArgs=@('cc','-std=c11','-O1','-Wall','-Wextra','-Werror','-UNDEBUG',"-I$root/gateway-core/include",
        "-I$root/out/sqlite/sqlite-amalgamation-3500400",$testPath)
    if($test -ne 'upload_admission'){$compilerArgs+="$root/out/vibration-tests/sqlite3.o"}
    if($test -in @('upload_queue_gateway','upload_timing_summary')){$compilerArgs+='-Wno-unused-function'}
    & $zig @compilerArgs -o "$build/$test.exe"
    if($LASTEXITCODE){throw "Compilation failed: $test"}
    $run=Join-Path $build ([guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory $run | Out-Null
    Push-Location $run
    try {& "$build/$test.exe";if($LASTEXITCODE){throw "Test failed: $test"}} finally {Pop-Location}
}
