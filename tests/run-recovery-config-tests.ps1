param([string]$NodeRoot=(Join-Path $PSScriptRoot '../../315-research'))
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$build=Join-Path $root 'out/recovery-config-tests'
New-Item -ItemType Directory -Force $build | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR="$root/out/zig-cache";$env:ZIG_LOCAL_CACHE_DIR="$root/out/zig-local"
$zig="$root/.codex-build-deps/ziglang/zig.exe"
$source=Get-Content -Raw -Encoding UTF8 "$NodeRoot/STM32_WPAN/App/app_zigbee.c"
$cases=@(
    @{Name='scalar_lease_timing';Functions=@()},
    @{Name='runtime_network_reset';Functions=@('ZbApp_RuntimeRestoreCallback','ZbApp_RestartSavedNetwork')},
    @{Name='queue_wake_sync';Functions=@('ZbApp_ServiceQueueWake')}
)
foreach($case in $cases) {
    $test=Get-Content -Raw -Encoding UTF8 "$PSScriptRoot/test_$($case.Name).c"
    foreach($name in $case.Functions) {
        $body=[regex]::Match($source,'(?ms)^static (?:void|int|uint8_t) '+$name+'\([^;{]*\)\r?\n\{.*?^\}').Value
        if(-not $body){throw "Cannot extract $name"};$test+="`n$body`n"
    }
    $path=Join-Path $build ($case.Name+'.c')
    [IO.File]::WriteAllText($path,$test,[Text.UTF8Encoding]::new($false))
    & $zig cc -std=c11 -O1 -Wall -Wextra -Werror -UNDEBUG "-I$NodeRoot/Core/Inc" $path -o "$build/$($case.Name).exe"
    if($LASTEXITCODE){throw "Compilation failed: $($case.Name)"}
    & "$build/$($case.Name).exe"
    if($LASTEXITCODE){throw "Test failed: $($case.Name)"}
}
