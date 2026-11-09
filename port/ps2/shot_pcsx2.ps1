# Run the ELF and save PNGs of the PCSX2 window: run_pcsx2.ps1 has no way to show a frame.
#
#   powershell -ExecutionPolicy Bypass -File port\ps2\shot_pcsx2.ps1 -Out <dir> -At 20,40 [-Seconds 60]
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$At = "30",                # comma-separated: -File does not parse arrays
    [int]$Seconds = 60,
    [string]$Renderer = "13",          # software: D3D12 hangs on the first real frame here
    [string]$Elf = (Join-Path $PSScriptRoot '../../build/ps2/out/djmax.elf')
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class W32 {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    public static IntPtr Biggest(uint want) {
        IntPtr best = IntPtr.Zero; long area = 0;
        EnumWindows((h, l) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid == want && IsWindowVisible(h)) {
                RECT r; GetWindowRect(h, out r);
                long a = (long)(r.R - r.L) * (r.B - r.T);
                if (a > area) { area = a; best = h; }
            }
            return true;
        }, IntPtr.Zero);
        return best;
    }
}
"@
[void][W32]::SetProcessDPIAware()
New-Item -ItemType Directory -Force -Path $Out | Out-Null

# Not -WindowStyle Hidden: the child inherits it and PCSX2's window never shows.
$runner = Start-Process powershell -PassThru -NoNewWindow -ArgumentList @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "$PSScriptRoot\run_pcsx2.ps1", '-Seconds', "$Seconds", '-Renderer', $Renderer, '-Elf', $Elf) -RedirectStandardOutput (Join-Path $Out "run.txt")

$t0 = Get-Date
foreach ($s in ($At -split ',' | ForEach-Object { [int]$_ } | Sort-Object)) {
    while (((Get-Date) - $t0).TotalSeconds -lt $s) { Start-Sleep -Milliseconds 200 }
    $p = Get-Process pcsx2-qt -ErrorAction SilentlyContinue | Select-Object -First 1
    $h = if ($p) { [W32]::Biggest([uint32]$p.Id) } else { [IntPtr]::Zero }
    if ($h -eq [IntPtr]::Zero) { Write-Host "no PCSX2 window at ${s}s"; continue }
    $r = New-Object W32+RECT
    [void][W32]::GetClientRect($h, [ref]$r)
    $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    # PrintWindow, not the screen: this must not take focus from whoever is using the PC.
    $dc = $g.GetHdc(); [void][W32]::PrintWindow($h, $dc, 3); $g.ReleaseHdc($dc)
    $file = Join-Path $Out ("shot_{0:d3}s.png" -f $s)
    $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    Write-Host "saved $file"
}
$runner.WaitForExit()
