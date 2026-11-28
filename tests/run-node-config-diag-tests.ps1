param([string]$NodeRoot=(Join-Path $PSScriptRoot '../../315-research'))
$ErrorActionPreference = 'Stop'
$OutputEncoding = [System.Text.UTF8Encoding]::new($false)
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'out/node-config-diag-tests'
New-Item -ItemType Directory -Force $build | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $root 'out/zig-cache'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $root 'out/zig-local'
$source = Get-Content -LiteralPath "$NodeRoot/STM32_WPAN/App/app_zigbee.c" -Raw -Encoding UTF8
if($source -match 'ZbApp_SendCommand\(\s*(?:ZB_CMD_CONFIG_QUERY|0x99U?)\s*,') {
    throw 'Application configuration queries must not be sent by the new firmware'
}
$testSource = Get-Content -LiteralPath "$PSScriptRoot/test_node_config_diag.c" -Raw -Encoding UTF8
foreach ($name in @('ZbApp_ConfigDiag','ZbApp_ConfigDiagQueued','ZbApp_SendCommand','ZbApp_TxTask',
    'ZbApp_OpenControlWindow','ZbApp_ServiceControlWindow','ZbApp_SendLinkStatus','ZbApp_ApplyMonitorSwitch',
    'ZbApp_ReportTimerCallback','ZbApp_StartConfigTransaction','ZbApp_StopConfigTransaction','ZbApp_ScheduleMonitorStandby')) {
    $pattern = '(?ms)^static (?:void|uint8_t|enum ZclStatusCodeT) '+[regex]::Escape($name)+'\([^;{]*\)\r?\n\{.*?^\}'
    $found = [regex]::Matches($source, $pattern)
    if ($found.Count -ne 1) { throw "Cannot extract node function: $name" }
    $body=$found[0].Value
    if($name -eq 'ZbApp_ScheduleMonitorStandby') {$body=$body.Replace('ZbApp_ScheduleMonitorStandby','ZbApp_ScheduleMonitorStandbyImpl')}
    $testSource += "`n" + $body + "`n"
}
foreach ($enabled in @(0,1)) {
    $exe = Join-Path $build "node-config-diag-$enabled.exe"
    $testSource | & "$root/.codex-build-deps/ziglang/zig.exe" cc -x c - -std=c11 -Wall -Wextra -Werror -UNDEBUG `
        "-DZB_CONFIG_TX_DIAG_ENABLED=$enabled" "-I$NodeRoot/Core/Inc" -o $exe
    if ($LASTEXITCODE) { throw "Node config diagnostic compilation failed: enabled=$enabled" }
    & $exe
    if ($LASTEXITCODE) { throw "Node config diagnostics failed: enabled=$enabled" }
}
