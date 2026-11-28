param([string]$CoordinatorRoot=(Join-Path $PSScriptRoot '../coordinator'))
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$build=Join-Path $root 'out/radio-diag-tests'
New-Item -ItemType Directory -Force $build | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR="$root/out/zig-cache";$env:ZIG_LOCAL_CACHE_DIR="$root/out/zig-local"
if((Get-FileHash "$root/gateway-core/include/radio_diag_protocol.h").Hash -ne (Get-FileHash "$CoordinatorRoot/Application/radio_diag_protocol.h").Hash){throw 'Radio diagnostic protocol headers differ'}
$source=Get-Content -Raw -Encoding UTF8 "$CoordinatorRoot/Application/gateway_bridge.c"
$function=[regex]::Match($source,'(?ms)^static uint8_t sendZcl\([^;{]*\)\r?\n\{.*?^\}').Value
if(!$function){throw 'Cannot extract actual AF submit function'}
$test=(Get-Content -Raw -Encoding UTF8 "$PSScriptRoot/test_radio_diag.c")+"`n$function`n"
[IO.File]::WriteAllText("$build/radio-diag.c",$test,[Text.UTF8Encoding]::new($false))
& "$root/.codex-build-deps/ziglang/zig.exe" cc -std=c11 -O1 -Wall -Wextra -Werror -UNDEBUG "-I$root/gateway-core/include" "-I$CoordinatorRoot/Application" "$build/radio-diag.c" -o "$build/radio-diag.exe"
if($LASTEXITCODE){throw 'Radio diagnostic test compilation failed'}
& "$build/radio-diag.exe"
if($LASTEXITCODE){throw 'Radio diagnostic tests failed'}
$source=Get-Content -Raw -Encoding UTF8 "$root/gateway-core/src/main.c"
$test=Get-Content -Raw -Encoding UTF8 "$PSScriptRoot/test_radio_diag_backend.c"
foreach($name in @('request_radio_diagnostics','handle_radio_diagnostic')) {
    $body=[regex]::Match($source,'(?ms)^static void '+$name+'\([^;{]*\)\r?\n\{.*?^\}').Value
    if(!$body){throw "Cannot extract backend $name"};$test+="`n$body`n"
}
[IO.File]::WriteAllText("$build/radio-backend.c",$test,[Text.UTF8Encoding]::new($false))
& "$root/.codex-build-deps/ziglang/zig.exe" cc -std=c11 -O1 -Wall -Wextra -Werror -UNDEBUG "-I$root/gateway-core/include" "$build/radio-backend.c" -o "$build/radio-backend.exe"
if($LASTEXITCODE){throw 'Backend radio test compilation failed'}
& "$build/radio-backend.exe"
if($LASTEXITCODE){throw 'Backend radio tests failed'}
