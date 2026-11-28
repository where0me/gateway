param([string]$NodeRoot=(Join-Path $PSScriptRoot '../../315-research'))
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$build=Join-Path $root 'out/node-recovery-tests'
New-Item -ItemType Directory -Force $build | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR=Join-Path $root 'out/zig-cache'
$env:ZIG_LOCAL_CACHE_DIR=Join-Path $root 'out/zig-local'
$OutputEncoding=[Text.UTF8Encoding]::new($false)
$source=Get-Content -LiteralPath "$NodeRoot/STM32_WPAN/App/app_zigbee.c" -Raw -Encoding UTF8
$testSource=Get-Content -LiteralPath "$PSScriptRoot/test_node_recovery.c" -Raw -Encoding UTF8
# Compile the actual firmware functions against a host stack/sequencer harness.
# Functions have their closing brace at column zero; declarations do not match.
foreach($name in @('ZbApp_AnnounceTask','ZbApp_VerifyParent','ZbApp_SetParentOnline','ZbApp_OpenControlWindow','ZbApp_StartAutonomousReporting')) {
    $pattern='(?ms)^static void '+[regex]::Escape($name)+'\([^\r\n]*\)\r?\n\{.*?^\}'
    $matchesFound=[regex]::Matches($source,$pattern)
    if($matchesFound.Count -ne 1){throw "Cannot extract firmware function: $name"}
    $testSource+="`n"+$matchesFound[0].Value+"`n"
}
$testSource | & "$root/.codex-build-deps/ziglang/zig.exe" cc -x c - -std=c11 -Wall -Wextra -Werror -UNDEBUG "-I$NodeRoot/Core/Inc" -o "$build/node-recovery.exe"
if($LASTEXITCODE){throw 'Node recovery test compilation failed'}
& "$build/node-recovery.exe"
if($LASTEXITCODE){throw 'Node recovery test failed'}
