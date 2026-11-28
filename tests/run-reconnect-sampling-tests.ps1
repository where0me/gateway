param([string]$NodeRoot=(Join-Path $PSScriptRoot '../../315-research'))
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$build=Join-Path $root 'out/reconnect-sampling-tests'
New-Item -ItemType Directory -Force $build|Out-Null
$env:ZIG_GLOBAL_CACHE_DIR="$root/out/zig-cache";$env:ZIG_LOCAL_CACHE_DIR="$root/out/zig-local"
$source=Get-Content -Raw -Encoding UTF8 "$NodeRoot/STM32_WPAN/App/app_zigbee.c"
$test=Get-Content -Raw -Encoding UTF8 "$PSScriptRoot/test_reconnect_sampling.c"
foreach($name in @('ZbApp_RequireSamplingSchedule','ZbApp_ResumeCachedUpload','ZbApp_ApplySamplingDeadline','ZbApp_WaitForSamplingDeadline','ZbApp_ArmSamplingTimer')) {
    $body=[regex]::Match($source,'(?ms)^static (?:void|uint8_t) '+$name+'\([^;{]*\)\r?\n\{.*?^\}').Value
    if(-not $body){throw "Cannot extract $name"};$test+="`n$body`n"
}
[IO.File]::WriteAllText("$build/node-reconnect.c",$test,[Text.UTF8Encoding]::new($false))
& "$root/.codex-build-deps/ziglang/zig.exe" cc -std=c11 -O1 -Wall -Wextra -Werror -UNDEBUG `
    "-I$NodeRoot/Core/Inc" "$build/node-reconnect.c" -o "$build/node-reconnect.exe"
if($LASTEXITCODE){throw 'Node reconnect compilation failed'}
& "$build/node-reconnect.exe"
if($LASTEXITCODE){throw 'Node reconnect failed'}
