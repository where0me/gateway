param([string]$NodeRoot=(Join-Path $PSScriptRoot '../../315-research'))
$ErrorActionPreference = 'Stop'
$OutputEncoding = [System.Text.UTF8Encoding]::new($false)
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'out/node-flash-tests'
New-Item -ItemType Directory -Force $build | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $root 'out/zig-cache'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $root 'out/zig-local'
$platform = "$NodeRoot/Middlewares/ST/STM32_WPAN/zigbee/platform"
$driver = Get-Content -LiteralPath "$platform/hw_flash.c" -Raw -Encoding UTF8
# Compile the actual driver body with host HAL/HSEM/SHCI peripherals. Firmware
# Debug/Release builds separately verify its real headers and target ABI.
$driver = [regex]::Replace($driver, '(?m)^#include[^\r\n]*', '')
$node = Get-Content -LiteralPath "$NodeRoot/STM32_WPAN/App/app_zigbee.c" -Raw -Encoding UTF8
$init = [regex]::Matches($node, '(?ms)^static bool ZbApp_NvmInit\(void\)\r?\n\{.*?^\}')
if ($init.Count -ne 1) { throw 'Cannot extract NVM initialization' }
$harness = Get-Content -LiteralPath "$PSScriptRoot/test_node_flash.c" -Raw -Encoding UTF8
$eeprom = Get-Content -LiteralPath "$platform/ee.c" -Raw -Encoding UTF8
$eeprom = [regex]::Replace($eeprom, '(?m)^#include[^\r\n]*', '')
foreach ($integration in @(0,1)) {
    $source = $harness + "`n" + $driver + "`n" + $init[0].Value
    if ($integration) { $source += "`n" + $eeprom }
    $exe = Join-Path $build "node-flash-$integration.exe"
    $source | & "$root/.codex-build-deps/ziglang/zig.exe" cc -x c - -std=c11 -Wall -Wextra -Werror -UNDEBUG `
        "-DFLASH_EEPROM_INTEGRATION=$integration" "-I$platform" -o $exe
    if ($LASTEXITCODE) { throw "Node Flash test compilation failed: integration=$integration" }
    & $exe
    if ($LASTEXITCODE) { throw "Node Flash tests failed: integration=$integration" }
}
