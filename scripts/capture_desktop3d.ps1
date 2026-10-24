#requires -Version 5.1
# Launches the real desktop app with --autoplay, waits for a few musical beats,
# and captures the client area of the BG Media SDL window. It deliberately
# finds the window by title because a console-subsystem process also owns a
# console window that must not appear in the screenshot.
param(
    [string]$Audio = "artifacts\fixtures\beat_123.wav",
    [string]$Vrm = "models\Model1.vrm",
    [string]$Out = "artifacts\desktop3d_playing.png",
    [int]$WaitSeconds = 5
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class BgWin {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int cmd);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT point);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc callback, IntPtr extraData);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr hWnd, StringBuilder text, int count);
    public static IntPtr FindWindow(uint processId, string titlePart) {
        IntPtr found = IntPtr.Zero;
        EnumWindows(delegate(IntPtr hWnd, IntPtr unused) {
            uint pid = 0;
            GetWindowThreadProcessId(hWnd, out pid);
            if (pid == processId && IsWindowVisible(hWnd)) {
                StringBuilder text = new StringBuilder(512);
                GetWindowText(hWnd, text, text.Capacity);
                if (text.ToString().IndexOf(titlePart, StringComparison.OrdinalIgnoreCase) >= 0) {
                    found = hWnd;
                    return false;
                }
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    public static void HideOtherWindows(uint processId, IntPtr keep) {
        EnumWindows(delegate(IntPtr hWnd, IntPtr unused) {
            uint pid = 0;
            GetWindowThreadProcessId(hWnd, out pid);
            if (pid == processId && hWnd != keep && IsWindowVisible(hWnd)) {
                ShowWindow(hWnd, 0); // SW_HIDE
            }
            return true;
        }, IntPtr.Zero);
    }
}
"@
[void][BgWin]::SetProcessDPIAware()

$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$exe = Join-Path $root "build\windows\bin\bgmedia_desktop.exe"
$audioPath = Join-Path $root $Audio
$vrmPath = Join-Path $root $Vrm
$outPath = Join-Path $root $Out
$arguments = '--file "{0}" --vrm "{1}" --3d --autoplay --no-cache' -f $audioPath, $vrmPath
$process = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $root -PassThru

try {
    $hwnd = [IntPtr]::Zero
    for ($i = 0; $i -lt 100; $i++) {
        Start-Sleep -Milliseconds 100
        $hwnd = [BgWin]::FindWindow([uint32]$process.Id, "BG Media")
        if ($hwnd -ne [IntPtr]::Zero) { break }
    }
    if ($hwnd -eq [IntPtr]::Zero) { throw "BG Media window did not appear." }

    [BgWin]::HideOtherWindows([uint32]$process.Id, $hwnd)
    [void][BgWin]::ShowWindow($hwnd, 9) # SW_RESTORE
    [void][BgWin]::SetForegroundWindow($hwnd)
    Start-Sleep -Seconds $WaitSeconds

    [BgWin+RECT]$rect = New-Object BgWin+RECT
    [void][BgWin]::GetClientRect($hwnd, [ref]$rect)
    $origin = New-Object BgWin+POINT
    $origin.X = 0
    $origin.Y = 0
    [void][BgWin]::ClientToScreen($hwnd, [ref]$origin)
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    if ($width -lt 100 -or $height -lt 100) { throw "Unexpected client size $width x $height" }

    $bitmap = New-Object System.Drawing.Bitmap($width, $height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CopyFromScreen($origin.X, $origin.Y, 0, 0, $bitmap.Size)
    $graphics.Dispose()
    New-Item -ItemType Directory -Force -Path (Split-Path $outPath) | Out-Null
    $bitmap.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()
    Write-Host "captured $outPath ($width x $height) while playing"

    [System.Windows.Forms.SendKeys]::SendWait(" ")
    Start-Sleep -Milliseconds 300
} finally {
    if (-not $process.HasExited) {
        [void]$process.CloseMainWindow()
        Start-Sleep -Milliseconds 700
        if (-not $process.HasExited) {
            $process.Kill()
        }
    }
}
