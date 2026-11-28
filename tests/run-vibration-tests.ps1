param(
    [string]$ZigExe,
    [string]$NodeRoot = (Join-Path $PSScriptRoot '../../315-research'),
    [string]$FusionNodeRoot = (Join-Path $PSScriptRoot '../../315-research'),
    [string]$SqliteDirectory
)
$ErrorActionPreference = "Stop"
$projectRoot = Split-Path $PSScriptRoot -Parent
if (-not $ZigExe) { $ZigExe = Join-Path $projectRoot ".codex-build-deps/ziglang/zig.exe" }
if (-not $SqliteDirectory) { $SqliteDirectory = Join-Path $projectRoot "out/sqlite/sqlite-amalgamation-3500400" }
$buildDirectory = Join-Path $projectRoot "out/vibration-tests"
New-Item -ItemType Directory -Force $buildDirectory | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $projectRoot "out/zig-cache"
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $projectRoot "out/zig-local"
function Invoke-Checked([string]$Executable, [string[]]$ArgumentList) {
    & $Executable @ArgumentList
    if ($LASTEXITCODE -ne 0) { throw "$Executable failed: $LASTEXITCODE" }
}
foreach ($name in @("vibration_codec.h", "vibration_tx.h")) {
    $gatewayHeader = Join-Path $projectRoot "gateway-core/include/$name"
    $nodeHeader = Join-Path $FusionNodeRoot "Core/Inc/$name"
    if ((Get-FileHash $gatewayHeader).Hash -ne (Get-FileHash $nodeHeader).Hash) {
        throw "Protocol copies differ: $name"
    }
}
$common = @("cc", "-std=c11", "-O1", "-g", "-I$(Join-Path $projectRoot 'gateway-core/include')")
$protocolExe = Join-Path $buildDirectory "protocol.exe"
Invoke-Checked $ZigExe ($common + @("-Wall", "-Wextra", "-Werror",
    (Join-Path $PSScriptRoot "test_vibration_v2.c"), "-o", $protocolExe))
Invoke-Checked $protocolExe @()
# Wire layouts match, but the node deliberately keeps its faster table-based CRC.
# Exercise the actual node header instead of requiring identical CRC source text.
$nodeProtocolExe = Join-Path $buildDirectory "node-protocol.exe"
Invoke-Checked $ZigExe @("cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
    "-I$(Join-Path $FusionNodeRoot 'Core/Inc')", "-I$(Join-Path $projectRoot 'gateway-core/include')",
    (Join-Path $PSScriptRoot "test_vibration_v2.c"), "-o", $nodeProtocolExe)
Invoke-Checked $nodeProtocolExe @()
$singleExe = Join-Path $buildDirectory "single-upload.exe"
Invoke-Checked $ZigExe ($common + @("-Wall", "-Wextra", "-Werror",
    (Join-Path $PSScriptRoot "test_single_upload.c"), "-o", $singleExe))
Invoke-Checked $singleExe @()
$sample = Join-Path $projectRoot "gateway-core/data/gateway.db-x-vibration_batches-29-samples.bin"
if (Test-Path -LiteralPath $sample) { Invoke-Checked $protocolExe @($sample) }

$flashExe = Join-Path $buildDirectory "flash.exe"
Invoke-Checked $ZigExe ($common + @("-Wall", "-Wextra", "-Werror",
    "-I$(Join-Path $PSScriptRoot 'stubs')", "-I$(Join-Path $NodeRoot 'Core/Inc')",
    (Join-Path $PSScriptRoot "test_vibration_flash.c"),
    (Join-Path $NodeRoot "Core/Src/vibration_monitor.c"), "-o", $flashExe))
Invoke-Checked $flashExe @()

$featuresExe = Join-Path $buildDirectory "deferred-features.exe"
Invoke-Checked $ZigExe ($common + @("-Wall", "-Wextra", "-Werror",
    "-I$(Join-Path $FusionNodeRoot 'Core/Inc')",
    (Join-Path $PSScriptRoot "test_vibration_features.c"), "-o", $featuresExe))
Invoke-Checked $featuresExe @()
$signalExe = Join-Path $buildDirectory "pending-signal.exe"
Invoke-Checked $ZigExe ($common + @("-Wall", "-Wextra", "-Werror",
    (Join-Path $PSScriptRoot "test_pending_signal.c"), "-o", $signalExe))
Invoke-Checked $signalExe @()
$fusionFlashExe = Join-Path $buildDirectory "fusion-store.exe"
Invoke-Checked $ZigExe ($common + @("-Wall", "-Wextra", "-Werror",
    "-I$(Join-Path $PSScriptRoot 'stubs')", "-I$(Join-Path $FusionNodeRoot 'Core/Inc')",
    (Join-Path $PSScriptRoot "test_fusion_vibration_store.c"),
    (Join-Path $FusionNodeRoot "Core/Src/zigbee_vibration_store.c"), "-o", $fusionFlashExe))
Invoke-Checked $fusionFlashExe @()

if (-not (Test-Path (Join-Path $SqliteDirectory "sqlite3.c"))) {
    throw "SQLite amalgamation missing. Pass -SqliteDirectory; no runtime database is used by tests."
}
$sqliteObject = Join-Path $buildDirectory "sqlite3.o"
Invoke-Checked $ZigExe @("cc", "-O1", "-c", (Join-Path $SqliteDirectory "sqlite3.c"), "-o", $sqliteObject)
foreach ($test in @("db", "gateway")) {
    $testExe = Join-Path $buildDirectory "$test.exe"
    Invoke-Checked $ZigExe ($common + @("-I$SqliteDirectory",
        (Join-Path $PSScriptRoot "test_vibration_$test.c"), $sqliteObject, "-o", $testExe))
    Invoke-Checked $testExe @()
}
$telemetryExe = Join-Path $buildDirectory "node-telemetry-migration.exe"
Invoke-Checked $ZigExe ($common + @("-I$SqliteDirectory",
    (Join-Path $PSScriptRoot "test_node_telemetry_migration.c"), $sqliteObject, "-o", $telemetryExe))
Push-Location $projectRoot
try { Invoke-Checked $telemetryExe @() } finally { Pop-Location }
$samplingExe = Join-Path $buildDirectory "node-sampling.exe"
Invoke-Checked $ZigExe ($common + @("-I$SqliteDirectory",
    (Join-Path $PSScriptRoot "test_node_sampling.c"), $sqliteObject, "-o", $samplingExe))
Push-Location $projectRoot
try { Invoke-Checked $samplingExe @() } finally { Pop-Location }
Write-Output "All vibration V2 and node telemetry tests passed. Only isolated test databases were used; device hardware was not accessed."
