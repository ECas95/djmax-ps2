param(
    [string]$Png = 'D:\DJMX\3ds\s3d_game.png',
    [int]$Lx0 = 375, [int]$Rx0 = 1645, [int]$Y0 = 128, [int]$Y1 = 732,
    [int[]]$Ranges = @(5, 170, 200, 345, 375, 520)
)
Add-Type -AssemblyName System.Drawing
$img = [Drawing.Bitmap]::FromFile($Png)
$bmpData = $img.LockBits((New-Object Drawing.Rectangle 0, 0, $img.Width, $img.Height), 'ReadOnly', 'Format32bppArgb')
$bytes = New-Object byte[] ($bmpData.Stride * $img.Height)
[Runtime.InteropServices.Marshal]::Copy($bmpData.Scan0, $bytes, 0, $bytes.Length)
$stride = $bmpData.Stride
$img.UnlockBits($bmpData); $img.Dispose()
function Gray([int]$x, [int]$y) { $o = $y * $stride + $x * 4; [int](0.11 * $bytes[$o] + 0.59 * $bytes[$o + 1] + 0.3 * $bytes[$o + 2]) }
for ($r = 0; $r -lt $Ranges.Count; $r += 2) {
    $x1 = $Ranges[$r]; $x2 = $Ranges[$r + 1]
    $best = [double]::MaxValue; $bd = 0
    foreach ($dx in -30..30) {
        $s = 0.0; $n = 0
        for ($y = $Y0 + 10; $y -lt $Y1 - 10; $y += 4) {
            for ($x = $x1; $x -lt $x2; $x += 2) {
                $s += [math]::Abs((Gray ($Lx0 + $x) $y) - (Gray ($Rx0 + $x + $dx) $y)); $n++
            }
        }
        $m = $s / $n
        if ($m -lt $best) { $best = $m; $bd = $dx }
    }
    "x $x1-$x2 : desplazamiento der-izq = $bd px (err {0:N1})" -f $best
}
