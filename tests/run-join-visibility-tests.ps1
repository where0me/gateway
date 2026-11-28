$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$build=Join-Path $root ('out/join-visibility-tests/'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $build|Out-Null
$env:ZIG_GLOBAL_CACHE_DIR=Join-Path $root 'out/zig-cache'
$env:ZIG_LOCAL_CACHE_DIR=Join-Path $root 'out/zig-local'
$zig=Join-Path $root '.codex-build-deps/ziglang/zig.exe'
$sql=Join-Path $root 'out/sqlite/sqlite-amalgamation-3500400'
& $zig cc -std=gnu11 -O1 -Wall -Wextra -Werror -UNDEBUG "-I$root/gateway-core/include" "-I$sql" `
    "$PSScriptRoot/test_join_visibility.c" "$root/out/vibration-tests/sqlite3.o" -o "$build/join-visibility.exe"
if($LASTEXITCODE){throw 'Join visibility compilation failed'}
Push-Location $build
try {& "$build/join-visibility.exe";if($LASTEXITCODE){throw 'Join visibility failed'}}
finally {Pop-Location}
