$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'out/node-card-batch-tests'
$sql = Join-Path $root 'out/sqlite/sqlite-amalgamation-3500400'
$zig = Join-Path $root '.codex-build-deps/ziglang/zig.exe'
New-Item -ItemType Directory -Force $build | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $root 'out/zig-cache'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $root 'out/zig-local'
$sqliteObject = Join-Path $root 'out/vibration-tests/sqlite3.o'
if (-not(Test-Path $sqliteObject)) { throw 'Run latest-readings database tests first to build SQLite' }
$source = Get-Content -LiteralPath "$root/gateway-core/src/main.c" -Raw -Encoding UTF8
$api = [regex]::Matches($source, '(?ms)^(?:static cJSON \*vibration_batch_json|void api_handle_vibration_latest)\([^;{]*\)\s*\r?\n\{.*?^\}')
if ($api.Count -ne 2) { throw 'Cannot extract vibration batch API and serializer' }
$harness = Get-Content -LiteralPath "$PSScriptRoot/test_node_card_batches.c" -Raw -Encoding UTF8
$harness = $harness.Replace('../gateway-core/src/db_manager.c', "$root/gateway-core/src/db_manager.c")
$test = Join-Path $build 'node-card-batches.c'
[IO.File]::WriteAllText($test, $harness + "`n" + (($api | ForEach-Object Value) -join "`n"), [Text.UTF8Encoding]::new($false))
$exe = Join-Path $build 'node-card-batches.exe'
& $zig cc -std=gnu11 -O1 -Wall -Wextra -UNDEBUG "-I$root/gateway-core/include" "-I$sql" $test "$root/gateway-core/src/cJSON.c" $sqliteObject -o $exe
if ($LASTEXITCODE) { throw 'Batch API compilation failed' }
& $exe
if ($LASTEXITCODE) { throw 'Batch API test failed' }
