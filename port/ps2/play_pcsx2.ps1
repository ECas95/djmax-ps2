# Interactive PCSX2 launcher. Run from Jugar-DJMAX.cmd at the project root.
param([switch]$BaseOnly)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$out = Join-Path $root $(if ($BaseOnly) { 'build\ps2\out-base' } else { 'build\ps2\out' })
$required = @('djmax.elf')
foreach ($name in @('opening', 'ending', 'ons', 'soundstorm')) {
    $required += "PS2_DATA\movie\$name.m2v", "PS2_DATA\movie\$name.at3p"
}
foreach ($name in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $out $name))) {
        throw "Falta un archivo necesario: $out\$name"
    }
}
$bgaManifest = Join-Path $out 'PS2_DATA\bga\manifest.json'
if (Test-Path -LiteralPath $bgaManifest) {
    foreach ($entry in (Get-Content -LiteralPath $bgaManifest -Raw | ConvertFrom-Json)) {
        $video = Join-Path $out ("PS2_DATA\bga\" + $entry.song + '.bgs')
        if (-not (Test-Path -LiteralPath $video)) { throw "Falta el BGA convertido: $video" }
    }
}
foreach ($name in @('autopad.txt', 'gedump.txt', 'gewatch.txt', 'snddump.txt', 'at3test.txt')) {
    if (Test-Path -LiteralPath (Join-Path $out $name)) {
        throw "Hay una prueba automatica activa: $out\$name. Renombrala antes de jugar."
    }
}
& (Join-Path $PSScriptRoot 'run_pcsx2.ps1') -Elf (Join-Path $out 'djmax.elf') -Seconds 0
