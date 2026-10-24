#requires -Version 5.1
<#
.SYNOPSIS
  Configure, build and test BG Media on Windows with the Visual Studio toolchain.

.DESCRIPTION
  Finds Visual Studio through vswhere, uses its bundled CMake/Ninja when
  available, and runs CMake + CTest from a vcvars64 environment. SDL3 is
  fetched by CMake on first configure if it is not installed.

.PARAMETER Config
  Release or Debug.

.PARAMETER CoreOnly
  Do not build the SDL3 desktop app.

.PARAMETER NoTests
  Skip the CTest step.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts/build_windows.ps1
#>
param(
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release",
    [switch]$CoreOnly,
    [switch]$NoTests
)

$ErrorActionPreference = "Stop"

$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildDir = Join-Path $root "build\windows"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = $null
if (Test-Path $vswhere) {
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
}
if (-not $vsPath -and $env:VSINSTALLDIR) {
    if (Test-Path (Join-Path $env:VSINSTALLDIR "VC\Auxiliary\Build\vcvars64.bat")) {
        $vsPath = $env:VSINSTALLDIR.TrimEnd('\')
    }
}
if (-not $vsPath) {
    # vswhere can be empty in preview/portable installs. Fall back to the
    # standard install roots and pick any edition that has the C++ toolchain.
    $roots = @(
        "C:\Program Files\Microsoft Visual Studio",
        "C:\Program Files (x86)\Microsoft Visual Studio"
    )
    foreach ($root in $roots) {
        if (-not (Test-Path $root)) { continue }
        foreach ($year in Get-ChildItem $root -Directory -ErrorAction SilentlyContinue) {
            foreach ($edition in Get-ChildItem $year.FullName -Directory -ErrorAction SilentlyContinue) {
                $candidate = $edition.FullName
                if (Test-Path (Join-Path $candidate "VC\Auxiliary\Build\vcvars64.bat")) {
                    $vsPath = $candidate
                    break
                }
            }
            if ($vsPath) { break }
        }
        if ($vsPath) { break }
    }
}
if (-not $vsPath) {
    throw "No Visual Studio installation with the MSVC C++ tools was found."
}

$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
$cmake = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
$ctest = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"

if (-not (Test-Path $cmake)) {
    $cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
    if (-not $cmakeCommand) {
        throw "CMake not found. Install CMake 3.24+ or use a Visual Studio install with bundled CMake."
    }
    $cmake = $cmakeCommand.Source
    $ctest = Join-Path (Split-Path $cmake) "ctest.exe"
}
if (-not (Test-Path $ninja)) {
    $ninjaCommand = Get-Command ninja -ErrorAction SilentlyContinue
    if (-not $ninjaCommand) {
        throw "Ninja not found. Install ninja or use a Visual Studio install with bundled Ninja."
    }
    $ninja = $ninjaCommand.Source
}

$desktop = if ($CoreOnly) { "OFF" } else { "ON" }
$tests = if ($NoTests) { "OFF" } else { "ON" }

# A temporary batch file avoids PowerShell/cmd quoting problems around paths
# with spaces and keeps vcvars64 in the same process as CMake.
$batch = Join-Path $env:TEMP "bgmedia_build_$PID.bat"
$lines = @(
    "@echo off",
    "call `"$vcvars`" >nul",
    "if errorlevel 1 exit /b 1",
    "`"$cmake`" -S `"$root`" -B `"$buildDir`" -G Ninja -DCMAKE_MAKE_PROGRAM=`"$ninja`" -DCMAKE_BUILD_TYPE=$Config -DBGMEDIA_BUILD_DESKTOP=$desktop -DBGMEDIA_BUILD_TESTS=$tests -DBGMEDIA_BUILD_TOOLS=ON -DBGMEDIA_BUILD_MINIAUDIO=ON",
    "if errorlevel 1 exit /b 1",
    "`"$cmake`" --build `"$buildDir`" --parallel",
    "if errorlevel 1 exit /b 1"
)
if (-not $NoTests) {
    $lines += "`"$ctest`" --test-dir `"$buildDir`" --output-on-failure"
    $lines += "if errorlevel 1 exit /b 1"
}
Set-Content -Path $batch -Value $lines -Encoding Default
try {
    & cmd.exe /d /c "`"$batch`""
    if ($LASTEXITCODE -ne 0) {
        throw "Build/test failed with exit code $LASTEXITCODE."
    }
} finally {
    Remove-Item $batch -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "BG Media built successfully." -ForegroundColor Green
Write-Host "  build:  $buildDir"
if (-not $CoreOnly) {
    Write-Host "  app:    $(Join-Path $root 'build\windows\bin\bgmedia_desktop.exe')"
}
if (-not $NoTests) {
    Write-Host "  tests:  $(Join-Path $root 'build\windows\bin\bgmedia_tests.exe')"
}
