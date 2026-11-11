# make_rgba.ps1 - convierte un PNG a .rgba crudo para el texture-pack del port.
#   .\make_rgba.ps1 -Png "logo.png" -Width 512 -Height 128 -Fp 57D2183C -OutDir "E:\djmax\tex"
# El PNG se encaja (letterbox transparente) en WxH preservando aspecto.
# La huella (Fp) sale del nombre del volcado: sdmc:/ge/tex/<addr>_<WxH>_<FP>.png
# (volcar con el flag sdmc:/djmx_texdump.flag). Bytes de salida: R,G,B,A.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Png,
    [Parameter(Mandatory)][int]$Width,
    [Parameter(Mandatory)][int]$Height,
    [Parameter(Mandatory)][string]$Fp,
    [string]$OutDir = '.'
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$src = [System.Drawing.Bitmap]::FromFile((Resolve-Path $Png))
$dst = New-Object System.Drawing.Bitmap($Width, $Height, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($dst)
$g.Clear([System.Drawing.Color]::FromArgb(0, 0, 0, 0))
$g.InterpolationMode = 'HighQualityBicubic'
$sc = [Math]::Min($Width / $src.Width, $Height / $src.Height)
$nw = [int]($src.Width * $sc); $nh = [int]($src.Height * $sc)
$g.DrawImage($src, [int](($Width - $nw) / 2), [int](($Height - $nh) / 2), $nw, $nh)
$g.Dispose(); $src.Dispose()
$rect = New-Object System.Drawing.Rectangle(0, 0, $Width, $Height)
$bd = $dst.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$bgra = New-Object byte[] ($Width * $Height * 4)
[System.Runtime.InteropServices.Marshal]::Copy($bd.Scan0, $bgra, 0, $bgra.Length)
$dst.UnlockBits($bd); $dst.Dispose()
$out = New-Object byte[] ($Width * $Height * 4)
for ($i = 0; $i -lt $Width * $Height; $i++) {
    $out[$i * 4]     = $bgra[$i * 4 + 2]
    $out[$i * 4 + 1] = $bgra[$i * 4 + 1]
    $out[$i * 4 + 2] = $bgra[$i * 4]
    $out[$i * 4 + 3] = $bgra[$i * 4 + 3]
}
New-Item -ItemType Directory -Force $OutDir | Out-Null
$path = Join-Path $OutDir ("{0}.rgba" -f $Fp.ToUpper())
[IO.File]::WriteAllBytes($path, $out)
Write-Host ("OK -> {0} ({1} bytes, {2}x{3})" -f $path, $out.Length, $Width, $Height)
