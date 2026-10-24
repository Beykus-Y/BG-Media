#requires -Version 5.1
$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    $vsPath = "C:\Program Files\Microsoft Visual Studio\2022\Preview"
}
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
$cmake = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
$buildDir = Join-Path $root "build\asan"
$msvcBin = (Get-ChildItem (Join-Path $vsPath "VC\Tools\MSVC") -Directory |
    Sort-Object Name -Descending | Select-Object -First 1).FullName + "\bin\Hostx64\x64"
$batch = Join-Path $env:TEMP "bgmedia_asan_$PID.bat"
$lines = @(
    "@echo off",
    "call `"$vcvars`" >nul",
    "if errorlevel 1 exit /b 1",
    "`"$cmake`" -S `"$root`" -B `"$buildDir`" -G Ninja -DCMAKE_MAKE_PROGRAM=`"$ninja`" -DCMAKE_BUILD_TYPE=Debug -DBGMEDIA_BUILD_DESKTOP=OFF -DBGMEDIA_BUILD_TESTS=ON -DBGMEDIA_BUILD_TOOLS=ON -DBGMEDIA_BUILD_MINIAUDIO=ON -DCMAKE_CXX_FLAGS=`"/fsanitize=address /Zi`" -DCMAKE_C_FLAGS=`"/fsanitize=address /Zi`" -DCMAKE_EXE_LINKER_FLAGS=`"/fsanitize=address`"",
    "if errorlevel 1 exit /b 1",
    "`"$cmake`" --build `"$buildDir`" --parallel",
    "if errorlevel 1 exit /b 1",
    "set PATH=%PATH%;$msvcBin",
    "`"$buildDir\bin\bgmedia_tests.exe`"",
    "if errorlevel 1 exit /b 1"
)
Set-Content -Path $batch -Value $lines -Encoding Default
try {
    & cmd.exe /d /c "`"$batch`""
    if ($LASTEXITCODE -ne 0) { throw "ASan tests failed with exit code $LASTEXITCODE." }
} finally {
    Remove-Item $batch -ErrorAction SilentlyContinue
}
