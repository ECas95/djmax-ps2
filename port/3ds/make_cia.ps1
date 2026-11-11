# -----------------------------------------------------------------------------
# make_cia.ps1 - empaqueta build/djmax3ds.elf como .cia (consola CFW) y .3ds/.cci
# (prueba en emulador). Requiere makerom y bannertool:
#   makerom:    https://github.com/3DSGuy/Project_CTR/releases
#   bannertool: https://github.com/carstene1ns/3ds-bannertool/releases
# Busca los .exe en -ToolsDir, en el PATH, o junto a este script.
# El build previo es obligatorio: .\build.ps1 (ver README para corpus/segmentos).
# -----------------------------------------------------------------------------
[CmdletBinding()]
param(
    [string]$ToolsDir = 'D:\DJMX\.tools\ctr'
)
$ErrorActionPreference = 'Stop'
$Port  = $PSScriptRoot
$Build = Join-Path $Port 'build'
$Cia   = Join-Path $Port 'cia'
$elf   = Join-Path $Build 'djmax3ds.elf'
if (-not (Test-Path $elf)) { Write-Host 'ERROR: falta build\djmax3ds.elf; corre build.ps1 primero' -ForegroundColor Red; exit 1 }

function Find-Tool([string]$name) {
    foreach ($c in @((Join-Path $ToolsDir $name), (Join-Path $Port $name))) {
        if (Test-Path $c) { return $c }
    }
    $hit = Get-ChildItem $ToolsDir -Recurse -Filter $name -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($hit) { return $hit.FullName }
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    Write-Host "ERROR: no se encuentra $name (ponlo en $ToolsDir o en el PATH)" -ForegroundColor Red
    exit 1
}
$makerom    = Find-Tool 'makerom.exe'
$bannertool = Find-Tool 'bannertool.exe'

$banner = Join-Path $Build 'banner.bnr'
$icon   = Join-Path $Build 'icon.icn'
& $bannertool makebanner -i (Join-Path $Cia 'banner.png') -a (Join-Path $Cia 'silence.wav') -o $banner | Out-Null
& $bannertool makesmdh -s 'DJMAX Fever 3DS' -l 'DJMAX Fever recomp (PSP -> 3DS). Requiere tus datos del juego en sdmc:/djmax/' -p 'djmax-recomp' -i (Join-Path $Cia 'icon.png') -o $icon | Out-Null
if (-not (Test-Path $banner) -or -not (Test-Path $icon)) { Write-Host 'ERROR: bannertool fallo' -ForegroundColor Red; exit 1 }

$rsf = Join-Path $Cia 'djmax3ds.rsf'
$out_cia = Join-Path $Build 'djmax3ds.cia'
$out_cci = Join-Path $Build 'djmax3ds.3ds'
& $makerom -f cia -o $out_cia -elf $elf -rsf $rsf -icon $icon -banner $banner -exefslogo -target t
if (-not (Test-Path $out_cia)) { Write-Host 'ERROR: makerom cia fallo' -ForegroundColor Red; exit 1 }
& $makerom -f cci -o $out_cci -elf $elf -rsf $rsf -icon $icon -banner $banner -exefslogo -target t
if (-not (Test-Path $out_cci)) { Write-Host 'ERROR: makerom cci fallo' -ForegroundColor Red; exit 1 }
Write-Host ("OK -> {0} ({1:N2} MB)" -f $out_cia, ((Get-Item $out_cia).Length / 1MB)) -ForegroundColor Green
Write-Host ("OK -> {0} ({1:N2} MB)" -f $out_cci, ((Get-Item $out_cci).Length / 1MB)) -ForegroundColor Green
Write-Host 'Consola: instala el .cia con FBI y pon tus datos en sdmc:/djmax/PSP_GAME/...'
Write-Host 'New 3DS: 804 MHz automatico. En consola real dumpea el DSP (app DSP1) para tener audio.'
