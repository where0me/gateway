param([string]$CoordinatorRoot=(Join-Path $PSScriptRoot '../coordinator'),
      [string]$NodeRoot=(Join-Path $PSScriptRoot '../../315-research'))
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$build=Join-Path $root 'out/upload-trace-tests'
New-Item -ItemType Directory -Force $build | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR="$root/out/zig-cache";$env:ZIG_LOCAL_CACHE_DIR="$root/out/zig-local"
$zig="$root/.codex-build-deps/ziglang/zig.exe"
foreach($copy in @("$CoordinatorRoot/Application/upload_trace.h","$NodeRoot/Core/Inc/upload_trace.h")){
    if((Get-FileHash "$root/gateway-core/include/upload_trace.h").Hash -ne (Get-FileHash $copy).Hash){throw 'Trace wire headers differ'}
}
foreach($name in @('upload_trace','upload_trace_backend','node_upload_trace','uart_checked','upload_trace_incoming')){
    $source=Get-Content -Raw -Encoding UTF8 "$PSScriptRoot/test_$name.c"
    if($name -eq 'uart_checked'){
        $actual=Get-Content -Raw -Encoding UTF8 "$root/gateway-core/src/uart_driver.c"
        foreach($fn in @('send_hex_frame_to_uart_checked','send_hex_frame_to_uart')){
            $body=[regex]::Match($actual,'(?ms)^(?:int|void) '+$fn+'\([^;{]*\)\s*\r?\n\{.*?^\}').Value
            if(!$body){throw "Cannot extract $fn"}
            $source+="`n$body`n"
        }
    }
    if($name -eq 'upload_trace_incoming'){
        $actual=Get-Content -Raw -Encoding UTF8 "$CoordinatorRoot/Application/gateway_bridge.c"
        foreach($fn in @('emitChecked','emit','incoming')){
            $body=[regex]::Match($actual,'(?ms)^static (?:bool|void) '+$fn+'\([^;{]*\)\r?\n\{.*?^\}').Value
            if(!$body){throw "Cannot extract $fn"};$source+="`n$body`n"
        }
    }
    [IO.File]::WriteAllText("$build/$name.c",$source,[Text.UTF8Encoding]::new($false))
    & $zig cc -std=c11 -O1 -Wall -Wextra -Werror -UNDEBUG "-I$root/gateway-core/include" "-I$CoordinatorRoot/Application" "-I$NodeRoot/Core/Inc" "$build/$name.c" -o "$build/$name.exe"
    if($LASTEXITCODE){throw "$name compilation failed"}
    & "$build/$name.exe"
    if($LASTEXITCODE){throw "$name tests failed"}
}
