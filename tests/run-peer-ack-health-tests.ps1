param([string]$NodeRoot=(Join-Path $PSScriptRoot '../../315-research'))
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$build=Join-Path $root 'out/peer-ack-health-tests'
New-Item -ItemType Directory -Force $build | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR="$root/out/zig-cache";$env:ZIG_LOCAL_CACHE_DIR="$root/out/zig-local"
$source=Get-Content -Raw -Encoding UTF8 "$NodeRoot/STM32_WPAN/App/app_zigbee.c"
$test=Get-Content -Raw -Encoding UTF8 "$PSScriptRoot/test_peer_ack_health.c"
foreach($name in @('ZbApp_UploadConfirm','ZbApp_UploadSend')) {
    $body=[regex]::Match($source,'(?ms)^static (?:void|int) '+$name+'\([^;{]*\)\r?\n\{.*?^\}').Value
    if(!$body){throw "Cannot extract $name"};$test+="`n$body`n"
}
$task=[regex]::Match($source,'(?ms)^static void ZbApp_UploadTask\(void\)\r?\n\{.*?^\}').Value
if(!$task -or $task -match 'upload_health\.pending\s*='){throw 'Upload task must not promote local/application failures to link recovery'}
$tail=$task.Substring($task.IndexOf("transfer_failed:" )+'transfer_failed:'.Length).Trim()
$tail=$tail.Substring(0,$tail.LastIndexOf('}')).Trim()
$test+="`nstatic void actual_transfer_failed(void)`n$tail`n"
[IO.File]::WriteAllText("$build/peer-ack-health.c",$test,[Text.UTF8Encoding]::new($false))
& "$root/.codex-build-deps/ziglang/zig.exe" cc -std=c11 -O1 -Wall -Wextra -Werror -UNDEBUG "-I$NodeRoot/Core/Inc" "$build/peer-ack-health.c" -o "$build/peer-ack-health.exe"
if($LASTEXITCODE){throw 'Peer ACK health test compilation failed'}
& "$build/peer-ack-health.exe"
if($LASTEXITCODE){throw 'Peer ACK health tests failed'}
