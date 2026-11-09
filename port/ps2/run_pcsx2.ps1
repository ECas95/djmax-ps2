# Run a PS2 ELF in PCSX2 and print the EE TTY lines it produced.
#
#   powershell -ExecutionPolicy Bypass -File port\ps2\run_pcsx2.ps1 [-Elf <path>] [-Seconds 25]
#
# PCSX2 only writes EE TTY output (the bytes the EE writes to 0x1000F180) to
# emulog.txt when [Logging] EnableEEConsole = true. This script flips that one
# key in the portable ini, runs the emulator with -batch -nogui -fastboot, then
# restores the ini byte-for-byte, so the user's configuration is not left
# changed. The program under test must write to the TTY itself; ps2sdk printf
# goes through IOP FILEIO and does not appear here.
param(
    [string]$Elf = (Join-Path $PSScriptRoot '../../build/ps2/out/djmax.elf'),
    [ValidateRange(0, 2147483)]
    [int]$Seconds = 25,             # 0: interactive session, until the user closes PCSX2
    [string]$Pcsx2Dir = (Join-Path $PSScriptRoot '../../.emu/.ps2/pcsx2-v2.8.2-windows-x64-Qt'),
    [string]$Renderer = "13",          # software. Auto (D3D12) hangs on the first displayed frame here; 11 is null
    [string]$GameDir = (Join-Path $PSScriptRoot '../../.game/DJMAX_Fever_(Patched)_[A912C17D]/PSP_GAME')
)

$ErrorActionPreference = "Stop"
$exe = Join-Path $Pcsx2Dir "pcsx2-qt.exe"
$ini = Join-Path $Pcsx2Dir "inis\PCSX2.ini"
$log = [System.IO.Path]::ChangeExtension($Elf, ".emulog.txt")
$outLog = [System.IO.Path]::ChangeExtension($Elf, ".pcsx2-stdout.txt")
$errLog = [System.IO.Path]::ChangeExtension($Elf, ".pcsx2-stderr.txt")

if (-not (Test-Path $exe)) { throw "pcsx2 not found: $exe" }
if (-not (Test-Path $ini)) { throw "PCSX2.ini not found: $ini" }
if (-not (Test-Path -LiteralPath $Elf)) { throw "ELF not found: $Elf" }
if (Get-Process -Name 'pcsx2-qt' -ErrorAction SilentlyContinue) {
    throw "Close PCSX2 before starting this launcher (its configuration is restored on exit)."
}

# The HLE reads disc0:/PSP_GAME/... as host:PSP_GAME/..., relative to the ELF.
# A junction, so never delete this folder recursively: use (Get-Item x).Delete().
$gameLink = Join-Path (Split-Path $Elf) "PSP_GAME"
if (-not (Test-Path $gameLink)) {
    # mklink, not New-Item: the [A912C17D] in the path is a wildcard to PowerShell.
    cmd /c mklink /J "$gameLink" "$GameDir" | Out-Null
    if (-not (Test-Path $gameLink)) { throw "junction failed: $gameLink" }
}

$backup = "$ini.djmax-$([guid]::NewGuid().ToString('N')).bak"
Copy-Item -LiteralPath $ini -Destination $backup
try {
    $cfg = Get-Content $ini -Raw
    function Set-IniValue([string]$Text, [string]$Section, [string]$Key, [string]$Value) {
        $sectionPattern = '(?ms)^\[' + [regex]::Escape($Section) + '\][^\r\n]*\r?\n(?:(?!^\[).)*'
        $keyPattern = '(?m)^' + [regex]::Escape($Key) + '\s*=.*$'
        if ($Text -match $sectionPattern) {
            return [regex]::Replace($Text, $sectionPattern, [System.Text.RegularExpressions.MatchEvaluator]{
                param($m)
                if ($m.Value -match $keyPattern) {
                    return [regex]::Replace($m.Value, $keyPattern, "$Key = $Value")
                }
                return $m.Value.TrimEnd() + "`r`n$Key = $Value`r`n`r`n"
            })
        }
        return $Text.TrimEnd() + "`r`n`r`n[$Section]`r`n$Key = $Value`r`n"
    }
    $cfg = Set-IniValue $cfg 'Logging' 'EnableEEConsole' 'true'
    $cfg = Set-IniValue $cfg 'EmuCore' 'HostFs' 'true'
    if ($Renderer) { $cfg = Set-IniValue $cfg 'EmuCore/GS' 'Renderer' $Renderer }
    Set-Content -Path $ini -Value $cfg -NoNewline

    Remove-Item $log, $outLog, $errLog -ErrorAction SilentlyContinue
    $p = Start-Process -FilePath $exe `
        -ArgumentList @('-portable', '-batch', '-nogui', '-fastboot', '-elf', $Elf,
                        '-logfile', $log, '-earlyconsolelog') `
        -PassThru -NoNewWindow -RedirectStandardOutput $outLog -RedirectStandardError $errLog
    if ($Seconds -eq 0) {
        Write-Host 'DJMAX listo. Cierra PCSX2 para terminar y restaurar su configuracion.'
        $p.WaitForExit()
    } elseif (-not $p.WaitForExit($Seconds * 1000)) {
        $p.Kill()
        Start-Sleep -Seconds 2
    }
}
finally {
    Copy-Item -LiteralPath $backup -Destination $ini -Force
    Remove-Item -LiteralPath $backup
}

Write-Host "=== EE TTY output from $(Split-Path $Elf -Leaf) ==="
if (Test-Path $log) {
    # PCSX2 prefixes every TTY line with a timestamp; print the payload only.
    Get-Content $log | Where-Object { $_ -match '^\[\s*\d+\.\d+\]\s+(?!microVU|Pad:|Set GS|Frame rate|UpdateVSyncRate|RegisterLibraryEntries|ELF |Media |Initializing )' }
} else {
    Write-Host "(no emulog produced)"
}
Write-Host "=== end ==="
