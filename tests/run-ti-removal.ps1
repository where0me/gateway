param([string]$Project=(Join-Path $PSScriptRoot '../coordinator'))
$ErrorActionPreference='Stop'
$testRoot=Split-Path $PSScriptRoot -Parent
$testOut=Join-Path $testRoot 'tmp/ti-removal-fix'
New-Item -ItemType Directory -Force $testOut | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR=Join-Path $testOut 'zig-global'
$env:ZIG_LOCAL_CACHE_DIR=Join-Path $testOut 'zig-local'
$testSource=(Join-Path $Project 'Application/gateway_removal.c').Replace('\','/')
& (Join-Path $testRoot '.codex-build-deps/ziglang/zig.exe') cc -std=c11 -Wall -Wextra -Werror ('-I'+(Join-Path $PSScriptRoot 'ti-removal-stubs')) ('-DGATEWAY_REMOVAL_SOURCE="'+$testSource+'"') (Join-Path $PSScriptRoot 'test_ti_removal.c') -o (Join-Path $testOut 'test-removal.exe')
if($LASTEXITCODE){throw 'Test build failed'}
& (Join-Path $testOut 'test-removal.exe')
if($LASTEXITCODE){throw 'Test failed'}
