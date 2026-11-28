param([string]$NodeRoot=(Join-Path $PSScriptRoot '../../315-research'))
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'out/download-clear-tests'
New-Item -ItemType Directory -Force $build | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $root 'out/zig-cache'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $root 'out/zig-local'
$main = Get-Content -LiteralPath "$NodeRoot/Core/Src/main.c" -Raw -Encoding UTF8
$commit = [regex]::Matches($main, '(?ms)^static int DownloadReset_Commit\(void \*context\)\r?\n\{.*?^\}')
if ($commit.Count -ne 1) { throw 'Cannot extract actual marker commit function' }
$body = $main.Substring($main.IndexOf('int main(void)'))
if ($body.IndexOf('DownloadReset_Boot();') -lt 0 -or
    $body.IndexOf('DownloadReset_Boot();') -gt $body.IndexOf('MX_APPE_Init();')) {
    throw 'External cleanup must precede CPU2/radio startup'
}
[xml]$launch = Get-Content -LiteralPath "$NodeRoot/STM32WB55RGV6-BLE-Zigbee Debug.launch" -Raw
$attributes = $launch.launchConfiguration
if (($attributes.booleanAttribute | Where-Object key -eq 'com.st.stm32cube.ide.mcu.debug.launch.incremental_flashing').value -ne 'false') {
    throw 'Incremental flashing must be disabled for repeat downloads'
}
$load = ($attributes.stringAttribute | Where-Object key -eq 'com.st.stm32cube.ide.mcu.debug.launch.loadList').value | ConvertFrom-Json
if ($load.fItems.Count -ne 1 -or !$load.fItems[0].fDownload -or !$load.fItems[0].fPath.EndsWith('STM32WB55RGV6-BLE-Zigbee.elf')) {
    throw 'Launch must download the complete dual-mode ELF'
}
if (($attributes.stringAttribute | Where-Object key -eq 'org.eclipse.cdt.debug.gdbjtag.core.initCommands').value -match 'mass_erase') {
    throw 'Internal mass erase must not be part of external cleanup'
}
$harness = Get-Content -LiteralPath "$PSScriptRoot/test_download_clear.c" -Raw -Encoding UTF8
$source = Join-Path $build 'download-clear.c'
[IO.File]::WriteAllText($source, $harness + "`n" + $commit[0].Value, [Text.UTF8Encoding]::new($false))
$exe = Join-Path $build 'download-clear.exe'
& "$root/.codex-build-deps/ziglang/zig.exe" cc -std=c11 -O1 -Wall -Wextra -Werror -UNDEBUG `
    -DDOWNLOAD_PLATFORM_TEST "-I$PSScriptRoot/stubs" "-I$NodeRoot/Core/Inc" $source `
    "$NodeRoot/Core/Src/zigbee_vibration_store.c" -o $exe
if ($LASTEXITCODE) { throw 'Download clear test compilation failed' }
& $exe
if ($LASTEXITCODE) { throw 'Download clear tests failed' }
Write-Output 'Boot order and CubeIDE download configuration PASS. No hardware was accessed.'
