# Compatibility entry point: the timed/concurrent upload implementation is retired.
# Keep historical scheduler sources for reference; test the active single-node path.
param([string]$ZigExe,[string]$SqliteDirectory,
      [string]$NodeRoot=(Join-Path $PSScriptRoot '../../315-research'))
$ErrorActionPreference='Stop'
Write-Output 'Single-node upload is active; running block transfer, Flash, UART and DB regressions.'
$argsForTests=@{FusionNodeRoot=$NodeRoot}
if($ZigExe){$argsForTests.ZigExe=$ZigExe}
if($SqliteDirectory){$argsForTests.SqliteDirectory=$SqliteDirectory}
& (Join-Path $PSScriptRoot 'run-vibration-tests.ps1') @argsForTests
