# -----------------------------------------------------------------------------
# build.ps1 - DJMAX Fever 3DS: corpus + platform layer -> .elf -> .3dsx
#
#   pwsh -File port/3ds/build.ps1                # full clean build
#   pwsh -File port/3ds/build.ps1 -Jobs 8        # parallel compile (default 8)
#   pwsh -File port/3ds/build.ps1 -Clean
#   pwsh -File port/3ds/build.ps1 -Instrumented  # build/ps2/src corpus + PSP_I
#
# Uses devkitPro at C:\devkitPro (override with -DevkitPro). Nothing outside
# D:\DJMX\port\3ds is written; build/eboot_reloc.bin and build/emit are read.
# -----------------------------------------------------------------------------
[CmdletBinding()]
param(
    [string]$DevkitPro = $(if ($env:DEVKITPRO -match '^[A-Za-z]:[\\/]') { $env:DEVKITPRO } else { 'C:\devkitPro' }),
    [int]$Jobs = 8,
    [ValidateSet('O2','Os')][string]$Opt = 'O2',
    [switch]$Clean,
    [switch]$Instrumented,
    [switch]$NoPackage
)

$ErrorActionPreference = 'Stop'
$Port   = $PSScriptRoot
$ParentDir = Split-Path -Parent $Port
# Supports both .../port/3ds and a standalone .../3ds from this ZIP.
$Root = if ((Split-Path $ParentDir -Leaf) -eq 'port') { Split-Path -Parent $ParentDir } else { $ParentDir }
$Build  = if ($Instrumented) { Join-Path $Port 'build\inst' } else { Join-Path $Port 'build' }
$Obj    = Join-Path $Build "obj-$Opt"
$Emit   = Join-Path $Root 'build\emit'
$Corpus = if ($Instrumented) { Join-Path $Build 'src' } elseif (Test-Path (Join-Path $Build 'emit_ovr')) { Join-Path $Build 'emit_ovr' } elseif (Test-Path (Join-Path $Build 'emit_ovr_off')) { Join-Path $Build 'emit_ovr_off' } else { $Emit }
$Eboot  = Join-Path $Root 'build\eboot_reloc.bin'

$Dka    = Join-Path $DevkitPro 'devkitARM'
$Gcc    = Join-Path $Dka 'bin\arm-none-eabi-gcc.exe'
$Gpp    = Join-Path $Dka 'bin\arm-none-eabi-g++.exe'
$Ctru   = Join-Path $DevkitPro 'libctru'
$Xt     = Join-Path $DevkitPro 'tools\bin\3dsxtool.exe'
$Smdh   = Join-Path $DevkitPro 'tools\bin\smdhtool.exe'

function Die([string]$msg) { Write-Host "ERROR: $msg" -ForegroundColor Red; exit 1 }

# ------------------------------------------------------------------ preflight
if (-not (Test-Path $Gcc))  { Die "devkitARM no encontrado en $Gcc. En Windows instala 3DS Development o pasa -DevkitPro 'D:\devkitPro'." }
if (-not (Test-Path (Join-Path $Ctru 'lib\libctru.a'))) { Die "no existe libctru.a" }
if (-not (Test-Path (Join-Path $Ctru 'include\3ds.h'))) { Die "no existe libctru/include/3ds.h" }
foreach ($required in @('include\citro3d.h','include\citro2d.h','lib\libcitro3d.a','lib\libcitro2d.a')) {
    if (-not (Test-Path (Join-Path $Ctru $required))) { Die "Falta el paquete 3ds-dev: $Ctru\$required" }
}
if (-not (Test-Path $Xt)) { Die "No se encuentra 3dsxtool en $Xt. Instala 3ds-tools." }
Write-Host "devkitPro Windows valido: $DevkitPro" -ForegroundColor Green
$UseExistingSegments = -not (Test-Path $Eboot)
if ($UseExistingSegments) {
    if (-not (Test-Path (Join-Path $Port 'build\seg0.bin')) -or
        -not (Test-Path (Join-Path $Port 'build\seg1.bin'))) {
        Die "faltan EBOOT y seg0.bin/seg1.bin; usa tus propios segmentos validos"
    }
    Write-Host "Usando segmentos existentes del ZIP; no requiere eboot_reloc.bin" -ForegroundColor Yellow
}
if (-not $Instrumented -and -not (Test-Path $Corpus)) { Die "no existe el corpus $Corpus" }

if ($Clean) {
    # Preserve emit_ovr and seg*.bin: those are the ONLY local copies in this ZIP.
    if (Test-Path $Obj) { Remove-Item -Recurse -Force $Obj }
    foreach ($n in @('djmax3ds.elf','djmax3ds.3dsx','djmax3ds.map','djmax3ds.smdh','module_image.S')) {
        $f = Join-Path $Build $n
        if (Test-Path $f) { Remove-Item -Force $f }
    }
    Write-Host "clean OK; se conservan corpus y segmentos"
    return
}
New-Item -ItemType Directory -Force $Build, $Obj | Out-Null
if ($Instrumented -and -not (Test-Path (Join-Path $Corpus 'djmax_funcs_000.c'))) {
    Write-Host "generating instrumented corpus in $Corpus"
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Root 'port\tools\make_corpus.ps1') -EmitDir $Emit -OutDir $Corpus
    if (-not (Test-Path (Join-Path $Corpus 'djmax_funcs_000.c'))) { Die "corpus generation failed" }
}

$arch   = @('-march=armv6k','-mtune=mpcore','-mfloat-abi=hard','-mtp=soft','-mword-relocations')
# O2: para .cia/.3ds (NCCH; mas rapido en consola). Os: para .3dsx (el loader
# 3dsx de Citra/Azahar rechaza el binario O2; el de consola no esta verificado).
$cflags = @("-$Opt",'-fomit-frame-pointer','-g','-ffunction-sections','-fdata-sections','-fno-strict-aliasing',
            '-fno-common','-w','-D__3DS__','-DPSPRECOMP_TRACE')
# El decoder ATRAC (third_party/at3_standalone_src) es C++ sin excepciones/RTTI.
$At3 = Join-Path $Root 'third_party\at3_standalone_src'
if (-not (Test-Path $At3)) { $At3 = Join-Path $Root '..\..\third_party\at3_standalone_src' }
if (-not (Test-Path $At3)) { Die "no se encuentra third_party/at3_standalone_src (decoder ATRAC)" }
$cxxflags = @("-$Opt",'-fomit-frame-pointer','-g','-ffunction-sections','-fdata-sections','-fno-strict-aliasing',
              '-fno-exceptions','-fno-rtti','-w','-D__3DS__',"-I$(Join-Path $Port 'at3_shim')","-I$At3")
$incs   = @("-I$Port", "-I$Port\include", "-I$Corpus", "-I$(Join-Path $Ctru 'include')", "-I$Build")
if ($Instrumented) { $incs += "-include"; $incs += (Join-Path $Port 'probe_3ds.h') }

# --------------------------------------------------- embedded module segments
$segs = @(
    @{ Name='seg0.bin'; Off=0x74;     Len=0xE15D4 },
    @{ Name='seg1.bin'; Off=0xE1648;  Len=0x11C4  }
)
if (-not $UseExistingSegments) {
    $eb = [IO.File]::ReadAllBytes($Eboot)
    foreach ($s in $segs) {
        $dst = Join-Path $Build $s.Name
        if (-not (Test-Path $dst) -or (Get-Item $dst).Length -ne $s.Len) {
            $buf = New-Object byte[] $s.Len
            [Array]::Copy($eb, $s.Off, $buf, 0, $s.Len)
            [IO.File]::WriteAllBytes($dst, $buf)
        }
    }
} else {
    foreach ($s in $segs) {
        $dst = Join-Path $Build $s.Name
        if ((Get-Item $dst).Length -ne $s.Len) { Die "segmento incorrecto: $dst" }
    }
}
$first = [BitConverter]::ToUInt32(([IO.File]::ReadAllBytes((Join-Path $Build 'seg0.bin'))), 0)
Write-Host ("segments: seg0 {0} B (first word 0x{1:X8}), seg1 {2} B" -f
            $segs[0].Len, $first, $segs[1].Len)

$asm = @"
    .section .rodata
    .align 4
    .globl seg0_start
seg0_start:
    .incbin "$($Build.Replace('\','/'))/seg0.bin"
    .globl seg0_end
seg0_end:
    .align 4
    .globl seg1_start
seg1_start:
    .incbin "$($Build.Replace('\','/'))/seg1.bin"
    .globl seg1_end
seg1_end:
    .align 4
"@
$asmPath = Join-Path $Build 'module_image.S'
Set-Content -Path $asmPath -Value $asm -NoNewline

# ------------------------------------------------------------------- sources
$sources = @()
$sources += Get-ChildItem (Join-Path $Corpus 'djmax_funcs_*.c') | Sort-Object Name | ForEach-Object FullName
$sources += Join-Path $Corpus 'djmax_register.c'
$LocalImports = Join-Path $Corpus 'djmax_imports.c'
if (-not (Test-Path $LocalImports)) { $LocalImports = Join-Path $Emit 'djmax_imports.c' }
$sources += $LocalImports
$plat = @('runtime_3ds.c','sched_3ds.c','hle_3ds.c','atrac_3ds.c','ps2_compat.c','ge_3ds.c',
          'video_3ds.c','audio_3ds.c','mpeg_3ds.c','imports_3ds.c','main.c')
# video_stub.c envuelve el pump de video 0x8B230: solo aplica cuando el corpus
# lo renombro a psp_real_0008B230 (emit_ovr); con el emit crudo ya existe.
$corpusHasPump = $null -ne (Get-ChildItem (Join-Path $Corpus 'djmax_funcs_*.c') |
    Select-String -Pattern 'void psp_func_0008B230' -List | Select-Object -First 1)
if ($corpusHasPump) {
    Write-Host "corpus define psp_func_0008B230: video_stub.c excluido" -ForegroundColor Yellow
} else {
    $plat += 'video_stub.c'
}
# Platform files are required; never silently omit a missing override.
foreach ($name in $plat) {
    $sourcePath = Join-Path $Port $name
    if (-not (Test-Path $sourcePath)) { Die "falta fuente necesaria del port: $sourcePath" }
}
$sources += $plat | ForEach-Object { Join-Path $Port $_ }
foreach ($extra in @('ge_draw.c','ge_tex.c')) {
    $p = Join-Path $Port $extra
    if (Test-Path $p) { $sources += $p }
}
foreach ($cpp in @('atrac3.cpp','atrac.cpp','atrac3plus.cpp','atrac3plusdec.cpp','atrac3plusdsp.cpp',
                   'compat.cpp','fft.cpp','get_bits.cpp','mem.cpp')) {
    $sources += Join-Path $At3 $cpp
}
$sources += Join-Path $Port 'at3_3ds.cpp'
$gt = Join-Path $Port 'ge_test'
if (Test-Path $gt) { $sources += Get-ChildItem $gt -Filter '*.c' | ForEach-Object FullName }
$sources += $asmPath
$sources = $sources | Where-Object { Test-Path $_ }
Write-Host "corpus: $(($sources | Where-Object { $_ -match 'djmax_funcs_' }).Count) units, $($sources.Count) sources"

# ------------------------------------------------------------------- compile
$common = $arch + $cflags + $incs
$objs = New-Object System.Collections.Generic.List[string]
$failed = 0
$running = New-Object System.Collections.Generic.List[object]
$i = 0

# Start-Process concatenates ArgumentList elements with spaces without automatically
# quoting paths. Build locations such as 'C:\Users\User Name' must be supported.
function Join-QuotedArgs([object[]]$values) {
    ($values | ForEach-Object {
        $v = [string]$_
        if ($v -match '[\s"]') { '"' + ($v -replace '"', '\"') + '"' } else { $v }
    }) -join ' '
}
function Start-One($src) {
    $base = [IO.Path]::GetFileNameWithoutExtension($src)
    $obj  = Join-Path $Obj "$base.o"
    $err  = Join-Path $Obj "$base.err"
    if ((Test-Path $obj) -and ((Get-Item $obj).LastWriteTime -ge (Get-Item $src).LastWriteTime)) {
        return @{ Obj=$obj; Proc=$null; Err=$err; Src=$src; Skip=$true }
    }
    $isCpp = $src.EndsWith('.cpp')
    $tool = if ($isCpp) { $Gpp } else { $Gcc }
    $useFlags = if ($src.EndsWith('.S')) { @() } elseif ($isCpp) { $arch + $cxxflags } else { $common }
    $args = @() + $useFlags + @('-c',$src,'-o',$obj)
    $p = Start-Process -FilePath $tool -ArgumentList (Join-QuotedArgs $args) -NoNewWindow -PassThru `
            -RedirectStandardError $err -RedirectStandardOutput (Join-Path $Obj "$base.out")
    return @{ Obj=$obj; Proc=$p; Err=$err; Src=$src; Skip=$false }
}

$sw = [Diagnostics.Stopwatch]::StartNew()
foreach ($src in $sources) {
    $i++
    $r = Start-One $src
    if ($r.Skip) { $objs.Add($r.Obj); continue }
    $running.Add($r)
    while ($running.Count -ge $Jobs) {
        Start-Sleep -Milliseconds 40
        for ($k = $running.Count - 1; $k -ge 0; $k--) {
            if ($running[$k].Proc.HasExited) {
                $r = $running[$k]; $running.RemoveAt($k)
                if ((Test-Path $r.Err) -and ((Get-Item $r.Err).Length -gt 0)) {
                    $failed++
                    Write-Host ("FAILED  {0}" -f (Split-Path $r.Src -Leaf)) -ForegroundColor Red
                    Get-Content $r.Err -TotalCount 12 | ForEach-Object { Write-Host "  $_" }
                } else { $objs.Add($r.Obj) }
            }
        }
    }
}
foreach ($r in $running) {
    $null = $r.Proc.WaitForExit(600000)
    if ((Test-Path $r.Err) -and ((Get-Item $r.Err).Length -gt 0)) {
        $failed++
        Write-Host ("FAILED  {0}" -f (Split-Path $r.Src -Leaf)) -ForegroundColor Red
        Get-Content $r.Err -TotalCount 12 | ForEach-Object { Write-Host "  $_" }
    } else { $objs.Add($r.Obj) }
}
# The parallel phase can lose a launch on this host (no .o and no .err); a
# deterministic second pass compiles whatever is missing, synchronously.
$objs = New-Object System.Collections.Generic.List[string]
foreach ($src in $sources) {
    $base = [IO.Path]::GetFileNameWithoutExtension($src)
    $objPath = Join-Path $Obj "$base.o"
    if (-not (Test-Path $objPath)) {
        Write-Host ("retry   {0}" -f (Split-Path $src -Leaf)) -ForegroundColor Yellow
        $errFile = Join-Path $Obj "$base.err"
        $eap = $ErrorActionPreference
        $ErrorActionPreference = 'SilentlyContinue'
        $isCpp = $src.EndsWith('.cpp')
        $tool = if ($isCpp) { $Gpp } else { $Gcc }
        $useFlags = if ($src.EndsWith('.S')) { @() } elseif ($isCpp) { $arch + $cxxflags } else { $common }
        & $tool @useFlags -c $src -o $objPath 2> $errFile
        $ErrorActionPreference = $eap
        if (-not (Test-Path $objPath)) { $failed++ }
    }
    $objs.Add($objPath)
}
Write-Host ("compiled: {0}/{1} objects in {2:N1}s, failures {3}" -f `
            $objs.Count, $sources.Count, $sw.Elapsed.TotalSeconds, $failed)
if ($failed -ne 0) { Die "compile errors (see $Obj\*.err)" }

# ---------------------------------------------------------------------- link
$elf = Join-Path $Build 'djmax3ds.elf'
$map = Join-Path $Build 'djmax3ds.map'
$linkArgs = @('-specs=3dsx.specs','-g') + $arch + @(
    "-Wl,-Map,$map", '-Wl,--gc-sections', '-o', $elf
) + $objs + @(
    "-L$(Join-Path $Ctru 'lib')", '-lcitro2d', '-lcitro3d', '-lctru', '-lm'
)
$linkLog = Join-Path $Build 'link.log'
$lp = Start-Process -FilePath $Gpp -ArgumentList (Join-QuotedArgs $linkArgs) -NoNewWindow -PassThru `
        -RedirectStandardError $linkLog -RedirectStandardOutput (Join-Path $Build 'link.out')
$null = $lp.WaitForExit(600000)
if (-not (Test-Path $elf) -or ((Get-Item $linkLog).Length -gt 0)) {
    Write-Host "LINK FAILED" -ForegroundColor Red
    Get-Content $linkLog -TotalCount 40 | ForEach-Object { Write-Host "  $_" }
    exit 1
}
$elfLen = (Get-Item $elf).Length
Write-Host ("linked: {0} ({1:N2} MB)" -f (Split-Path $elf -Leaf), ($elfLen / 1MB))

# -------------------------------------------------------------------- package
if ($NoPackage) { return }
$smdhPath = Join-Path $Build 'djmax3ds.smdh'
$icon = Join-Path $Ctru 'default_icon.png'
if ((Test-Path $Smdh) -and (Test-Path $icon)) {
    $sp = Start-Process -FilePath $Smdh -ArgumentList (Join-QuotedArgs @('--create','DJMAX Fever 3DS',
        'Recompiled PSP module','DJMX port',$icon,$smdhPath)) -NoNewWindow -PassThru
    $null = $sp.WaitForExit(60000)
}
$3dsx = Join-Path $Build 'djmax3ds.3dsx'
$xtArgs = @($elf, $3dsx)
if (Test-Path $smdhPath) { $xtArgs += "--smdh=$smdhPath" }
$xp = Start-Process -FilePath $Xt -ArgumentList (Join-QuotedArgs $xtArgs) -NoNewWindow -PassThru
$null = $xp.WaitForExit(60000)
if (-not (Test-Path $3dsx)) { Die "3dsxtool failed" }
Write-Host ("OK -> {0} ({1:N2} MB)" -f $3dsx, ((Get-Item $3dsx).Length / 1MB))
