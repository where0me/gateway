$ErrorActionPreference = 'Stop'
$OutputEncoding = [System.Text.UTF8Encoding]::new($false)
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'out/config-diag-tests'
New-Item -ItemType Directory -Force $build | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $root 'out/zig-cache'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $root 'out/zig-local'
$source = Get-Content -LiteralPath "$root/gateway-core/src/main.c" -Raw -Encoding UTF8
$testSource = Get-Content -LiteralPath "$PSScriptRoot/test_config_diag.c" -Raw -Encoding UTF8
foreach ($name in @('read_le16', 'write_le16', 'write_le32', 'trace_config_uart',
                     'build_monitor_config_payload', 'send_monitor_config_scoped', 'handle_cmd_99_config_query')) {
    $pattern = '(?ms)^(?:static )?(?:void|int|uint16_t) ' + [regex]::Escape($name) + '\([^;{]*\)\r?\n\{.*?^\}'
    $found = [regex]::Matches($source, $pattern)
    if ($found.Count -ne 1) { throw "Cannot extract backend function: $name" }
    $testSource += "`n" + $found[0].Value + "`n"
}
# The RX hook must precede existing revoked-node filtering, otherwise missing
# queries could be incorrectly blamed on the radio instead of local filtering.
$dispatcher = [regex]::Match($source, '(?ms)^void dispatch_uart_command\([^;{]*\)\s*\{.*?^\}').Value
if (-not $dispatcher -or $dispatcher.IndexOf('trace_config_uart(cmd, payload, len)') -lt 0 -or
    $dispatcher.IndexOf('trace_config_uart(cmd, payload, len)') -gt $dispatcher.IndexOf('retry_removed_device_by_short')) {
    throw 'Config trace does not precede revoked-node filtering'
}
if ($source -notmatch 'config_diag_start\(getenv\("ZIGBEE_CONFIG_DIAG"\), mg_millis\(\)\)') {
    throw 'Diagnostic startup switch missing'
}
$testSource | & "$root/.codex-build-deps/ziglang/zig.exe" cc -x c - -std=c11 -Wall -Wextra -Werror -UNDEBUG `
    "-I$root/gateway-core/include" -o "$build/config-diag.exe"
if ($LASTEXITCODE) { throw 'Config diagnostic test compilation failed' }
& "$build/config-diag.exe"
if ($LASTEXITCODE) { throw 'Config diagnostic tests failed' }
