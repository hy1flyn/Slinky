#requires -Version 5.1
<#
    Builds the Slinky client and the injector on Windows.

    Usage:
        .\build.ps1              build client + injector, copy Slinky.exe to dist\
        .\build.ps1 -Clean       remove the build directories first
        .\build.ps1 -Client      client DLL only
        .\build.ps1 -Injector    injector only (needs the client DLL)
        .\build.ps1 -Jobs 8      parallelism (default: all cores)

    Requires on PATH or in a default install location:
        LLVM (clang-cl, lld-link, llvm-rc, llvm-mt), CMake 3.20+, Ninja
    For the injector: MinGW-w64 g++ (the Windows build of the injector uses it).
#>
[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$Client,
    [switch]$Injector,
    [int]$Jobs = 0
)

$ErrorActionPreference = 'Stop'
$Root = $PSScriptRoot
$BuildClient   = Join-Path $Root 'build\cmake'
$BuildInjector = Join-Path $Root 'build\injector'
$Dist          = Join-Path $Root 'dist'

function Write-Step([string]$Text) {
    Write-Host ''
    Write-Host "==> $Text" -ForegroundColor Cyan
}

function Write-Ok([string]$Text) {
    Write-Host "    $Text" -ForegroundColor Green
}

function Write-Bad([string]$Text) {
    Write-Host "    $Text" -ForegroundColor Red
}

# Adds a directory to PATH for this process if it exists and is not there yet.
function Add-PathEntry([string]$Dir) {
    if (-not $Dir) { return }
    if (-not (Test-Path -LiteralPath $Dir)) { return }
    $full = (Resolve-Path -LiteralPath $Dir).Path
    $parts = $env:Path -split ';' | Where-Object { $_ }
    foreach ($p in $parts) {
        if ($p.TrimEnd('\') -ieq $full.TrimEnd('\')) { return }
    }
    $env:Path = $full + ';' + $env:Path
}

# Finds a tool either on PATH or in the usual install locations.
function Find-Tool([string]$Name, [string[]]$Fallbacks) {
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($f in $Fallbacks) {
        if ($f -and (Test-Path -LiteralPath $f)) { return $f }
    }
    return $null
}

function Get-LLVMDir {
    $candidates = @(
        'C:\Program Files\LLVM\bin',
        'C:\Program Files (x86)\LLVM\bin',
        (Join-Path $env:ProgramFiles 'LLVM\bin')
    )
    if ($env:LLVM_DIR) { $candidates = @((Join-Path $env:LLVM_DIR 'bin')) + $candidates }
    foreach ($c in $candidates) {
        if ($c -and (Test-Path -LiteralPath (Join-Path $c 'clang-cl.exe'))) { return $c }
    }
    # Visual Studio ships a copy of the LLVM tools under its VC folder.
    $vs = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio' -Directory -ErrorAction SilentlyContinue
    foreach ($v in $vs) {
        $hit = Get-ChildItem $v.FullName -Recurse -Filter 'clang-cl.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($hit) { return $hit.DirectoryName }
    }
    return $null
}

function Get-NinjaPath {
    $c = Get-Command ninja -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    foreach ($p in @(
        'C:\Program Files\CMake\bin\ninja.exe',
        'C:\Strawberry\c\bin\ninja.exe',
        'C:\ProgramData\chocolatey\bin\ninja.exe')) {
        if (Test-Path -LiteralPath $p) { return $p }
    }
    return $null
}

function Get-CMakePath {
    $c = Get-Command cmake -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    foreach ($p in @(
        'C:\Program Files\CMake\bin\cmake.exe',
        'C:\Program Files (x86)\CMake\bin\cmake.exe')) {
        if (Test-Path -LiteralPath $p) { return $p }
    }
    return $null
}

function Get-MinGWPath {
    # The injector is built with MinGW-w64 g++; Strawberry Perl ships one.
    $c = Get-Command 'g++.exe' -ErrorAction SilentlyContinue
    if ($c -and $c.Source -match 'mingw|strawberry|msys') { return $c.Source }
    foreach ($p in @(
        'C:\Strawberry\c\bin\g++.exe',
        'C:\msys64\mingw64\bin\g++.exe',
        'C:\mingw64\bin\g++.exe')) {
        if (Test-Path -LiteralPath $p) { return $p }
    }
    return $null
}

# ---------------------------------------------------------------------------
Write-Step 'Checking the toolchain'

$cmake  = Get-CMakePath
$ninja  = Get-NinjaPath
$llvmDir = Get-LLVMDir

if (-not $cmake) { Write-Bad 'cmake not found. Install CMake 3.20+ and put it on PATH.'; exit 1 }
Add-PathEntry (Split-Path $cmake)

if (-not $ninja) { Write-Bad 'ninja not found. Install it or pass its directory on PATH.'; exit 1 }
Add-PathEntry (Split-Path $ninja)

if (-not $llvmDir) { Write-Bad 'LLVM not found (clang-cl.exe). Install LLVM and put bin\ on PATH.'; exit 1 }
Add-PathEntry $llvmDir

$clang = Find-Tool 'clang-cl.exe' @()
if (-not $clang) { Write-Bad 'clang-cl.exe not found in the LLVM directory.'; exit 1 }

Write-Ok "cmake     $cmake"
Write-Ok "ninja     $ninja"
Write-Ok "LLVM      $llvmDir"

$threads = if ($Jobs -gt 0) { $Jobs } else { [Environment]::ProcessorCount }

# ---------------------------------------------------------------------------
if ($Clean) {
    Write-Step 'Cleaning'
    foreach ($d in @($BuildClient, $BuildInjector)) {
        if (Test-Path -LiteralPath $d) {
            Remove-Item -LiteralPath $d -Recurse -Force
            Write-Ok "removed $d"
        }
    }
}

$doClient   = -not $Injector -or $Client
$doInjector = -not $Client   -or $Injector

# ---------------------------------------------------------------------------
if ($doClient) {
    Write-Step 'Building the client DLL'

    $toolchain = Join-Path $Root 'cmake\toolchain-windows-clang-cl.cmake'
    if (-not (Test-Path -LiteralPath $toolchain)) {
        Write-Bad "toolchain file missing: $toolchain"
        exit 1
    }

    $cacheFile = Join-Path $BuildClient 'CMakeCache.txt'
    $needConfigure = $true
    if (Test-Path -LiteralPath $cacheFile) {
        # Re-configure only when the generator or compiler changed, so plain rebuilds are fast.
        $cache = Get-Content -LiteralPath $cacheFile -Raw
        if ($cache -match 'CMAKE_MAKE_PROGRAM:.*ninja' -and $cache -match 'CMAKE_TOOLCHAIN_FILE') {
            $needConfigure = $false
        }
    }

    if (-not $needConfigure) {
        Write-Ok 'reusing the existing configuration'
    } else {
        & $cmake -S $Root -B $BuildClient -G Ninja `
            "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
            "-DCMAKE_MAKE_PROGRAM=$ninja" `
            '-DCMAKE_BUILD_TYPE=Release'
        if ($LASTEXITCODE -ne 0) { Write-Bad 'cmake configure failed'; exit $LASTEXITCODE }
    }

    & $cmake --build $BuildClient --parallel $threads
    if ($LASTEXITCODE -ne 0) { Write-Bad 'client build failed'; exit $LASTEXITCODE }

    $dll = Join-Path $BuildClient 'SlinkyNative.dll'
    if (-not (Test-Path -LiteralPath $dll)) {
        Write-Bad "client build produced no DLL at $dll"
        exit 1
    }
    $size = (Get-Item -LiteralPath $dll).Length
    Write-Ok ("SlinkyNative.dll  {0:N0} bytes" -f $size)
}

# ---------------------------------------------------------------------------
if ($doInjector) {
    Write-Step 'Building the injector'

    $dll = Join-Path $BuildClient 'SlinkyNative.dll'
    if (-not (Test-Path -LiteralPath $dll)) {
        Write-Bad 'the client DLL is missing; build it first (run without -Injector)'
        exit 1
    }

    $gxx = Get-MinGWPath
    if (-not $gxx) {
        Write-Bad 'g++.exe (MinGW-w64) not found. The injector build needs it.'
        exit 1
    }
    Add-PathEntry (Split-Path $gxx)
    Write-Ok "g++       $gxx"

    $injectorSrc = Join-Path $Root 'injector'
    & $cmake -S $injectorSrc -B $BuildInjector -G Ninja `
        "-DCMAKE_CXX_COMPILER=$gxx" `
        "-DCMAKE_MAKE_PROGRAM=$ninja" `
        "-DSLINKY_PAYLOAD=$dll" `
        '-DCMAKE_BUILD_TYPE=Release'
    if ($LASTEXITCODE -ne 0) { Write-Bad 'injector configure failed'; exit $LASTEXITCODE }

    & $cmake --build $BuildInjector --parallel $threads
    if ($LASTEXITCODE -ne 0) { Write-Bad 'injector build failed'; exit $LASTEXITCODE }

    $exe = Join-Path $BuildInjector 'Slinky.exe'
    if (-not (Test-Path -LiteralPath $exe)) {
        Write-Bad "injector build produced no executable at $exe"
        exit 1
    }

    if (-not (Test-Path -LiteralPath $Dist)) { New-Item -ItemType Directory -Path $Dist | Out-Null }
    $out = Join-Path $Dist 'Slinky.exe'
    Copy-Item -LiteralPath $exe -Destination $out -Force

    $size = (Get-Item -LiteralPath $out).Length
    Write-Ok ("Slinky.exe        {0:N0} bytes" -f $size)
}

# ---------------------------------------------------------------------------
Write-Step 'Done'

$exe = Join-Path $Dist 'Slinky.exe'
if (Test-Path -LiteralPath $exe) {
    $h = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    Write-Ok "dist\Slinky.exe  $h"
} else {
    $dll = Join-Path $BuildClient 'SlinkyNative.dll'
    if (Test-Path -LiteralPath $dll) {
        $h = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash
        Write-Ok "build\cmake\SlinkyNative.dll  $h"
    }
}
