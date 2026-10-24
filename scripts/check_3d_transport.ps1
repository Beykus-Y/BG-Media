#requires -Version 5.1
param(
    [string]$Audio = "artifacts\fixtures\beat_123.wav",
    [string]$Vrm = "models\Model1.vrm"
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class BgWindow {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT point);
}
"@
[void][BgWindow]::SetProcessDPIAware()
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$exe = Join-Path $root "build\windows\bin\bgmedia_desktop.exe"
$arguments = '--file "{0}" --vrm "{1}" --3d --autoplay --no-cache' -f (Join-Path $root $Audio), (Join-Path $root $Vrm)
$process = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $root -PassThru
function Capture-Window([string]$name) {
    $process.Refresh()
    $hwnd = $process.MainWindowHandle
    if ($hwnd -eq [IntPtr]::Zero) { throw "no window" }
    [void][BgWindow]::SetForegroundWindow($hwnd)
    Start-Sleep -Milliseconds 300
    [BgWindow+RECT]$rect = New-Object BgWindow+RECT
    [void][BgWindow]::GetClientRect($hwnd, [ref]$rect)
    $origin = New-Object BgWindow+POINT
    [void][BgWindow]::ClientToScreen($hwnd, [ref]$origin)
    $w = $rect.Right - $rect.Left
    $h = $rect.Bottom - $rect.Top
    $bitmap = New-Object System.Drawing.Bitmap($w, $h)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CopyFromScreen($origin.X, $origin.Y, 0, 0, $bitmap.Size)
    $graphics.Dispose()
    $path = Join-Path $root ("artifacts\" + $name + ".png")
    $bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()
    Write-Host "captured $path"
}
try {
    for ($i = 0; $i -lt 80; $i++) {
        Start-Sleep -Milliseconds 100
        $process.Refresh()
        if ($process.MainWindowHandle -ne [IntPtr]::Zero) { break }
    }
    [void][BgWindow]::SetForegroundWindow($process.MainWindowHandle)
    Start-Sleep -Seconds 3
    Capture-Window "desktop3d_playing_check"
    [System.Windows.Forms.SendKeys]::SendWait(" ")
    Start-Sleep -Seconds 1
    Capture-Window "desktop3d_paused_check"
    [System.Windows.Forms.SendKeys]::SendWait("{RIGHT}")
    Start-Sleep -Seconds 1
    Capture-Window "desktop3d_seek_check"
    [System.Windows.Forms.SendKeys]::SendWait(" ")
    Start-Sleep -Seconds 1
} finally {
    if (-not $process.HasExited) {
        [void]$process.CloseMainWindow()
        Start-Sleep -Milliseconds 700
        if (-not $process.HasExited) { $process.Kill() }
    }
}
