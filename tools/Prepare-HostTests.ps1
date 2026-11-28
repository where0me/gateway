param(
    [Parameter(Mandatory=$true)][string]$ZigDirectory,
    [Parameter(Mandatory=$true)][string]$SqliteDirectory
)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$zigSource=(Resolve-Path -LiteralPath $ZigDirectory).Path
$sqlSource=(Resolve-Path -LiteralPath $SqliteDirectory).Path
foreach($file in @('zig.exe','lib')) { if(!(Test-Path -LiteralPath (Join-Path $zigSource $file))){throw "Missing Zig component: $file"} }
foreach($file in @('sqlite3.c','sqlite3.h')) { if(!(Test-Path -LiteralPath (Join-Path $sqlSource $file))){throw "Missing SQLite source: $file"} }
$zigTarget=Join-Path $root '.codex-build-deps/ziglang'
$sqlTarget=Join-Path $root 'out/sqlite/sqlite-amalgamation-3500400'
New-Item -ItemType Directory -Force $zigTarget,$sqlTarget,(Join-Path $root 'out/vibration-tests')|Out-Null
if([IO.Path]::GetFullPath($zigSource) -ne [IO.Path]::GetFullPath($zigTarget)) {
    foreach($item in Get-ChildItem -LiteralPath $zigSource -Force){Copy-Item -LiteralPath $item.FullName -Destination $zigTarget -Recurse -Force}
}
foreach($file in @('sqlite3.c','sqlite3.h','sqlite3ext.h')) {
    $source=Join-Path $sqlSource $file
    if((Test-Path -LiteralPath $source) -and [IO.Path]::GetFullPath($sqlSource) -ne [IO.Path]::GetFullPath($sqlTarget)) {
        Copy-Item -LiteralPath $source -Destination (Join-Path $sqlTarget $file) -Force
    }
}
$env:ZIG_GLOBAL_CACHE_DIR=Join-Path $root 'out/zig-cache'
$env:ZIG_LOCAL_CACHE_DIR=Join-Path $root 'out/zig-local'
& (Join-Path $zigTarget 'zig.exe') cc -std=c11 -O1 -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION -c (Join-Path $sqlTarget 'sqlite3.c') -o (Join-Path $root 'out/vibration-tests/sqlite3.o')
if($LASTEXITCODE){throw 'SQLite host compilation failed'}
Write-Output 'Host dependencies prepared. Run the required tests from the repository root.'
