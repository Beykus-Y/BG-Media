#requires -Version 5.1
<#
  Compiles the SDL_GPU HLSL shaders to DXIL and embeds them as C headers.
  Requires the DirectX Shader Compiler (dxc.exe) shipped with the Windows SDK
  or Visual Studio. Generated headers are committed so a normal build does not
  need dxc.

  Usage:
    powershell -ExecutionPolicy Bypass -File scripts/compile_shaders.ps1
#>
$ErrorActionPreference = "Stop"

$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$shaderDir = Join-Path $root "adapters\sdlgpu\shaders"
$generatedDir = Join-Path $shaderDir "generated"

$dxc = $null
$kitRoots = @(
    "${env:ProgramFiles(x86)}\Windows Kits\10\bin",
    "${env:ProgramFiles}\Windows Kits\10\bin"
)
foreach ($kitRoot in $kitRoots) {
    if (-not (Test-Path $kitRoot)) { continue }
    $candidate = Get-ChildItem $kitRoot -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName "x64\dxc.exe" } |
        Where-Object { Test-Path $_ } |
        Select-Object -First 1
    if ($candidate) { $dxc = $candidate; break }
}
if (-not $dxc) {
    $command = Get-Command dxc -ErrorAction SilentlyContinue
    if ($command) { $dxc = $command.Source }
}
if (-not $dxc) {
    throw "dxc.exe was not found. Install the Windows SDK or Visual Studio C++ tools."
}

New-Item -ItemType Directory -Force -Path $generatedDir | Out-Null

$shaders = @(
    @{ Name = "mesh.vert";        Target = "vs_6_0" },
    @{ Name = "mesh.frag";        Target = "ps_6_0" },
    @{ Name = "background.vert";  Target = "vs_6_0" },
    @{ Name = "background.frag";  Target = "ps_6_0" },
    @{ Name = "floor.vert";       Target = "vs_6_0" },
    @{ Name = "floor.frag";       Target = "ps_6_0" }
)

foreach ($shader in $shaders) {
    $input = Join-Path $shaderDir "$($shader.Name).hlsl"
    $dxil = Join-Path $generatedDir "$($shader.Name).dxil"
    & $dxc -T $shader.Target -E main -Fo $dxil $input
    if ($LASTEXITCODE -ne 0) {
        throw "dxc failed for $($shader.Name)"
    }

    $bytes = [IO.File]::ReadAllBytes($dxil)
    $symbol = ($shader.Name -replace '\.', '_') + "_dxil"
    $header = Join-Path $generatedDir (($shader.Name -replace '\.', '_') + ".h")
    $builder = New-Object System.Text.StringBuilder
    [void]$builder.AppendLine("#pragma once")
    [void]$builder.AppendLine("// Generated from $($shader.Name).hlsl by scripts/compile_shaders.ps1; do not edit.")
    [void]$builder.AppendLine("static const unsigned char ${symbol}[] = {")
    $line = "  "
    for ($i = 0; $i -lt $bytes.Length; ++$i) {
        $line += ("0x{0:x2}," -f $bytes[$i])
        if ((($i + 1) % 12) -eq 0) {
            [void]$builder.AppendLine($line)
            $line = "  "
        }
    }
    if ($line.Trim().Length -gt 0) {
        [void]$builder.AppendLine($line)
    }
    [void]$builder.AppendLine("};")
    [IO.File]::WriteAllText($header, $builder.ToString())
    Remove-Item $dxil -Force
    Write-Host "compiled $($shader.Name) -> $([IO.Path]::GetFileName($header))"
}

Write-Host "Shaders compiled into $generatedDir" -ForegroundColor Green
