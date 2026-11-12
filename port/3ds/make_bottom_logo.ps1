param(
    [string]$Logo = 'D:\DJMX\3ds\texdump\081A1990_512x128_57D2183C.png',
    [string]$OutRgba = "$env:APPDATA\Azahar\sdmc\djmax\tex\bottom_logo.rgba",
    [string]$OutPng = 'D:\DJMX\3ds\tests\bottom_logo_preview.png',
    [single]$LogoAlpha = 0.50,
    [single]$TextAlpha = 0.55
)
Add-Type -AssemblyName System.Drawing
$W = 512; $H = 256
$bmp = New-Object Drawing.Bitmap $W, $H, ([Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [Drawing.Graphics]::FromImage($bmp)
$g.Clear([Drawing.Color]::FromArgb(0, 0, 0, 0))
$g.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::HighQuality
$g.TextRenderingHint = [Drawing.Text.TextRenderingHint]::AntiAliasGridFit

# logo regular con opacidad
$img = [Drawing.Image]::FromFile($Logo)
$cm = New-Object Drawing.Imaging.ColorMatrix
$cm.Matrix33 = $LogoAlpha
$ia = New-Object Drawing.Imaging.ImageAttributes
$ia.SetColorMatrix($cm)
$g.DrawImage($img, (New-Object Drawing.Rectangle 0, 0, 512, 128), 0, 0, $img.Width, $img.Height, [Drawing.GraphicsUnit]::Pixel, $ia)
$img.Dispose()

# texto "DJMAX ENCORE" con tracking ancho
$font = New-Object Drawing.Font 'Arial Black', 25, ([Drawing.FontStyle]::Regular), ([Drawing.GraphicsUnit]::Pixel)
$brush = New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb([int](255 * $TextAlpha), 170, 185, 210))
$text = 'DJMAX ENCORE'
$track = 7.0
$fmt = [Drawing.StringFormat]::GenericTypographic
$total = 0.0
$widths = foreach ($ch in $text.ToCharArray()) {
    $cw = if ($ch -eq ' ') { 14.0 } else { $g.MeasureString([string]$ch, $font, 1000, $fmt).Width }
    $total += $cw + $track
    $cw
}
$x = ($W - ($total - $track)) / 2.0
$y = 146.0
$i = 0
foreach ($ch in $text.ToCharArray()) {
    if ($ch -ne ' ') { $g.DrawString([string]$ch, $font, $brush, [single]$x, [single]$y, $fmt) }
    $x += $widths[$i] + $track
    $i++
}
$g.Dispose()

$bmp.Save($OutPng, [Drawing.Imaging.ImageFormat]::Png)

$rect = New-Object Drawing.Rectangle 0, 0, $W, $H
$d = $bmp.LockBits($rect, [Drawing.Imaging.ImageLockMode]::ReadOnly, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
$n = $W * $H * 4
$src = New-Object byte[] $n
[Runtime.InteropServices.Marshal]::Copy($d.Scan0, $src, 0, $n)
$bmp.UnlockBits($d)
$dst = New-Object byte[] $n
for ($p = 0; $p -lt $n; $p += 4) {          # BGRA (alpha recta) -> RGBA
    $dst[$p]     = $src[$p + 2]
    $dst[$p + 1] = $src[$p + 1]
    $dst[$p + 2] = $src[$p]
    $dst[$p + 3] = $src[$p + 3]
}
New-Item -ItemType Directory -Path (Split-Path $OutRgba) -Force | Out-Null
[IO.File]::WriteAllBytes($OutRgba, $dst)
$bmp.Dispose()
"OK $OutRgba ($n bytes)"
