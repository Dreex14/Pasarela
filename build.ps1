# Compila Pasarela.exe (portátil, sin dependencias) con llvm-mingw.
# Uso:  powershell -ExecutionPolicy Bypass -File build.ps1 [-Debug]
param([switch]$Debug, [string]$Out = "")
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$tc = Get-ChildItem -Directory (Join-Path $root 'tools') -Filter 'llvm-mingw-*' | Select-Object -First 1
if (-not $tc) { throw 'No se encontró llvm-mingw en tools/. Descárgalo de https://github.com/mstorsjo/llvm-mingw/releases (ucrt-x86_64).' }
$bin = Join-Path $tc.FullName 'bin'
$cxx = Join-Path $bin 'x86_64-w64-mingw32-clang++.exe'
$rc  = Join-Path $bin 'x86_64-w64-mingw32-windres.exe'
$dist = Join-Path $root 'dist'
$obj  = Join-Path $root 'build'
New-Item -ItemType Directory -Force $dist, $obj | Out-Null

$ico = Join-Path $root 'assets\pasarela.ico'
if (-not (Test-Path $ico)) {
    New-Item -ItemType Directory -Force (Join-Path $root 'assets') | Out-Null
    python -I (Join-Path $root 'tools\make_icon.py') $ico
}

Push-Location (Join-Path $root 'src')
try {
    & $rc -O coff -o (Join-Path $obj 'app.res.o') app.rc
    if ($LASTEXITCODE) { throw 'windres falló' }

    $opt = if ($Debug) { @('-O0', '-g') } else { @('-O2', '-DNDEBUG') }
    $flags = @('-std=c++20', '-municode', '-mwindows', '-Wall', '-Wno-unknown-pragmas', '-Wno-missing-braces',
               '-D_WIN32_WINNT=0x0A00', '-DWINVER=0x0A00', '-static', '-s') + $opt
    $libs = @('-ld3d11', '-ldxgi', '-ld2d1', '-ldwrite', '-ld3dcompiler_47', '-lmfplat', '-lmf', '-lmfreadwrite',
              '-lmfuuid', '-lole32', '-loleaut32', '-luuid', '-lavrt', '-lcomctl32', '-luxtheme', '-ldwmapi',
              '-lshell32', '-lcfgmgr32', '-lpropsys', '-lwinmm', '-lshlwapi', '-lgdi32', '-luser32')
    if ($Debug) { $flags = $flags | Where-Object { $_ -ne '-s' } }
    $src = Get-ChildItem *.cpp | ForEach-Object { $_.Name }
    $exe = if ($Out) { $Out } else { Join-Path $dist "Pasarela.exe" }
    & $cxx @flags @src (Join-Path $obj "app.res.o") @libs -o $exe
    if ($LASTEXITCODE) { throw 'La compilación falló' }
} finally { Pop-Location }
Get-Item $exe | Select-Object Name, Length, LastWriteTime
