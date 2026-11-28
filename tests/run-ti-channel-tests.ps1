param(
    [string]$TiRoot = (Join-Path $PSScriptRoot '../coordinator'),
    [string]$ZigExe
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
if (-not $ZigExe) { $ZigExe = Join-Path $projectRoot '.codex-build-deps/ziglang/zig.exe' }
$bridge = Get-Content -LiteralPath (Join-Path $TiRoot 'Application/gateway_bridge.c') -Raw
$start = $bridge.IndexOf('static uint8_t nwkUpdate(')
$end = $bridge.IndexOf('static void snapshotNext(', $start)
if ($start -lt 0 -or $end -le $start) { throw 'Cannot locate bridge channel functions' }
$functions = $bridge.Substring($start, $end-$start)
if (-not $functions.Contains('static void channelTick(')) { throw 'Missing channel tick implementation' }
$opts = Get-Content -LiteralPath (Join-Path $TiRoot 'Stack/Config/f8wcoord.opts') -Raw
if ($opts -notmatch '(?m)^-DZDO_MGMT_NWKUPDATE_REQUEST\s*$') { throw 'Network update request is not enabled' }
$mainLoop = $bridge.Substring($end)
if (-not $mainLoop.Contains('channelTick(now);')) { throw 'Application loop does not run channel tick' }
$outputDir = Join-Path $projectRoot 'out/vibration-tests'
New-Item -ItemType Directory -Force $outputDir | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $projectRoot 'out/zig-cache'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $projectRoot 'out/zig-local'
$testExe = Join-Path $outputDir 'ti-channel.exe'
$source = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'test_ti_channel.c') -Raw) + "`n" + $functions
# Feed the source directly to the compiler: no generated source file or SDK edits.
$source | & $ZigExe cc -x c - -std=c11 -O1 -UNDEBUG -Wall -Wextra -Werror -o $testExe
if ($LASTEXITCODE -ne 0) { throw 'TI channel host compilation failed' }
& $testExe
if ($LASTEXITCODE -ne 0) { throw 'TI channel host tests failed' }
