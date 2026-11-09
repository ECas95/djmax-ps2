#!/usr/bin/env bash
# DJMAX Fever --> PS2: build djmax.elf with ee-gcc, no make.
#
#   ./build.sh game    # full instrumented corpus + module image + scheduler
#   ./build.sh subset  # 10 corpus units + imports + runtime + main (source check)
#   ./build.sh full    # uninstrumented corpus, no scheduler (first milestone)
#
# game mode (default) does four extra things over `full`:
#   - regenerates build/ps2/src with port/ps2/make_ps2_corpus.ps1: one
#     PSP_I(0xADDR) per guest instruction and the .cplinit loader fixup;
#   - extracts the EBOOT's two PT_LOAD segments and embeds them with objcopy;
#   - compiles ps2_sched.c / ps2_hle.c and the PS2 import thunks;
#   - force-includes port/ps2/ps2_probe.h so PSP_I exists.
#
# set REGEN=0 to skip the corpus regeneration step.
set -uo pipefail

P=${PS2DEV:-$HOME/ps2dev}
SDK="$P/ps2sdk"
CC="$P/ee/bin/mips64r5900el-ps2-elf-gcc"
CXX="$P/ee/bin/mips64r5900el-ps2-elf-g++"
NM="$P/ee/bin/mips64r5900el-ps2-elf-nm"
OBJCOPY="$P/ee/bin/mips64r5900el-ps2-elf-objcopy"
READELF="$P/ee/bin/mips64r5900el-ps2-elf-readelf"
SIZE="$P/ee/bin/mips64r5900el-ps2-elf-size"
OBJDUMP="$P/ee/bin/mips64r5900el-ps2-elf-objdump"

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
PORT="$ROOT/port/ps2"
EMIT="$ROOT/build/emit"
SRC="$ROOT/build/ps2/src"          # instrumented + fixed corpus
OUT="$ROOT/build/ps2/out"
OBJDIR="$OUT/obj"
EBOOT="$ROOT/build/eboot_reloc.bin"   # written by make_ps2_corpus.ps1 (REGEN=1)
MODE="${1:-game}"
BASE_ONLY=${BASE_ONLY:-1}
if [ "$BASE_ONLY" = 1 ]; then
    SRC="$ROOT/build/ps2/src-base"
    OUT="$ROOT/build/ps2/out-base"
    OBJDIR="$OUT/obj"
fi

mkdir -p "$OBJDIR"
START=$(date +%s)

# -G0: the corpus uses absolute guest addresses, so no gp-relative small data.
# -w: machine-generated code; only errors matter.
CFLAGS="-D_EE -G0 -Os -fno-strict-aliasing -w"
if [ "$BASE_ONLY" = 1 ]; then CFLAGS="$CFLAGS -DDJMAX_BASE_ONLY"; fi
INCS="-I$PORT -I$PORT/include -I$OBJDIR -I$SDK/ee/include -I$SDK/common/include -I$SDK/ports/include"
# ATRAC3/3+ decoders (third_party, FFmpeg via PPSSPP, C++): -O2, they are the hot path of music.
AT3="$ROOT/third_party/at3_standalone_src"
CXXFLAGS="-D_EE -G0 -O2 -fsingle-precision-constant -fno-exceptions -fno-rtti -fno-strict-aliasing -w -I$PORT/at3_shim -I$AT3"

echo "== DJMAX PS2 build ($MODE) =="
echo "cc: $($CC --version | head -1)"

# ---- corpus + embedded module image -------------------------------------
if [ "$MODE" = game ]; then
    if [ "${REGEN:-1}" = 1 ]; then
        echo "-- regenerating instrumented corpus (make_ps2_corpus.ps1)"
        GEN_ARGS=()
        if [ "$BASE_ONLY" = 1 ]; then GEN_ARGS+=(-BaseOnly); fi
        powershell.exe -NoProfile -ExecutionPolicy Bypass \
            -File "$(wslpath -w "$PORT/make_ps2_corpus.ps1")" "${GEN_ARGS[@]}" 2>&1 | tail -5
        [ -f "$SRC/djmax_funcs_000.c" ] || { echo "corpus generation failed"; exit 1; }
    fi
    INCS="$INCS -I$SRC"

    # The two PT_LOAD segments, embedded with .incbin in an assembly file.
    # objcopy -I binary produces an ELF whose ABI flags ld rejects against the
    # EE toolchain ("ABI is incompatible with that of the selected emulation");
    # .incbin keeps one object format and one ABI.
    echo "-- extracting EBOOT segments"
    dd if="$EBOOT" of="$OBJDIR/seg0.bin" bs=1M iflag=skip_bytes,count_bytes skip=$((0x74)) count=$((0xE15D4)) status=none
    dd if="$EBOOT" of="$OBJDIR/seg1.bin" bs=1M iflag=skip_bytes,count_bytes skip=$((0xE1648)) count=$((0x11C4)) status=none
    # .data, 64-byte aligned: once copied into g_module at boot, seg0's 923 KB are
    # reused as the ATRAC decoder arena (ps2_at3.cpp).
    cat > "$OBJDIR/segdata.S" <<EOF
    .section .data
    .align 6
    .globl seg0_start
seg0_start:
    .incbin "$OBJDIR/seg0.bin"
    .globl seg0_end
seg0_end:
    .align 4
    .globl seg1_start
seg1_start:
    .incbin "$OBJDIR/seg1.bin"
    .globl seg1_end
seg1_end:
    .align 4
EOF
    "$CC" -c "$OBJDIR/segdata.S" -o "$OBJDIR/segdata.o" || { echo "segdata assembly failed"; exit 1; }
fi

# ---- source lists --------------------------------------------------------
FILES=()
CORPUS_FLAGS=""
if [ "$MODE" = game ]; then
    while IFS= read -r f; do FILES+=("$f"); done < <(ls "$SRC"/djmax_funcs_*.c | sort)
    FILES+=("$SRC/ps2_imports.c")
    REG_SRC="$SRC/djmax_register.c"
    CORPUS_FLAGS="-include $PORT/ps2_probe.h"
    [ "${PS2_TRACE:-0}" = 1 ] && CORPUS_FLAGS="$CORPUS_FLAGS -DPS2_TRACE"
elif [ "$MODE" = full ]; then
    while IFS= read -r f; do FILES+=("$f"); done < <(ls "$EMIT"/djmax_funcs_*.c | sort)
    FILES+=("$EMIT/djmax_imports.c")
    REG_SRC="$EMIT/djmax_register.c"
    INCS="$INCS -I$EMIT"
else
    for n in 000 001 002 003 004 005 006 007 008 083; do
        FILES+=("$EMIT/djmax_funcs_$n.c")
    done
    FILES+=("$EMIT/djmax_imports.c")
    REG_SRC=""
    INCS="$INCS -I$EMIT"
fi
FILES+=("$PORT/runtime_min.c" "$PORT/main.c" "$PORT/ps2_sched.c" "$PORT/ps2_hle.c" "$PORT/ps2_ge.c" "$PORT/ps2_audio.c" "$PORT/ps2_libc.c" "$PORT/ps2_zlib.c" "$PORT/ps2_snd.c" "$PORT/ps2_mpeg.c" "$PORT/ps2_savedata.c" "$PORT/ps2_songs.c" "$PORT/ps2_bga.c")
# IOP modules loaded at run time (ps2_snd.c): libsd + audsrv, embedded as C arrays.
IRXC="$OBJDIR/irx_gen.c"
if [ ! -f "$IRXC" ] || [ "$SDK/iop/irx/audsrv.irx" -nt "$IRXC" ]; then
    # aligned: SifExecModuleBuffer DMAs the array to the IOP, and SIF DMA needs 16-byte alignment
    ( cd "$SDK/iop/irx" && xxd -i audsrv.irx ) | sed 's/^unsigned char \([a-z_]*\)\[\] = {/unsigned char \1[] __attribute__((aligned(64))) = {/' > "$IRXC" || exit 1
fi
FILES+=("$IRXC")
for f in atrac3 atrac atrac3plus atrac3plusdec atrac3plusdsp compat fft get_bits mem; do FILES+=("$AT3/$f.cpp"); done
FILES+=("$PORT/ps2_at3.cpp")

echo "corpus units: $((${#FILES[@]} - 3))"

# ---- compile -------------------------------------------------------------
OBJS=()
FAILED=0
i=0
for f in "${FILES[@]}"; do
    i=$((i + 1))
    base=$(basename "${f%.c}"); base="${base%.cpp}"
    o="$OBJDIR/$base.o"
    # Only the instrumented corpus needs PSP_I; compile it with the probe forced in.
    XF=""
    case "$f" in
        "$SRC"/djmax_funcs_*.c) XF="$CORPUS_FLAGS" ;;
    esac
    # Incremental: an up-to-date object with no error log is reused.
    if [ -f "$o" ] && [ "$o" -nt "$f" ] && [ -f "$OBJDIR/$base.err" ] && [ ! -s "$OBJDIR/$base.err" ]; then
        OBJS+=("$o"); continue
    fi
    case "$f" in
        *.cpp) COMPILE=("$CXX" $CXXFLAGS $INCS) ;;
        *)     COMPILE=("$CC" $CFLAGS $INCS $XF) ;;
    esac
    if ! "${COMPILE[@]}" -c "$f" -o "$o" 2> "$OBJDIR/$base.err"; then
        echo "FAILED  $f"
        sed -n '1,25p' "$OBJDIR/$base.err"
        FAILED=1
        continue
    fi
    OBJS+=("$o")
    [ $((i % 8)) -eq 0 ] && echo "  compiled $i/${#FILES[@]}"
done
echo "compiled: ${#OBJS[@]}/${#FILES[@]} objects"

if [ "$FAILED" -ne 0 ]; then
    echo "STOP: compile errors above (see $OBJDIR/*.err)"
    exit 1
fi

if [ "$MODE" = game ]; then
    OBJS+=("$OBJDIR/segdata.o")
fi

# ---- registration -------------------------------------------------------
if [ -n "$REG_SRC" ]; then
    echo "registering from $(basename "$REG_SRC")"
    "$CC" $CFLAGS $INCS -c "$REG_SRC" -o "$OBJDIR/djmax_register.o" 2> "$OBJDIR/djmax_register.err"
    if [ $? -ne 0 ]; then
        echo "FAILED  $REG_SRC"; sed -n '1,25p' "$OBJDIR/djmax_register.err"; exit 1
    fi
    OBJS+=("$OBJDIR/djmax_register.o")
else
    REG_C="$OUT/register_gen.c"
    {
        echo "/* generated by build.sh: register what this link actually defines */"
        echo '#include "runtime_min.h"'
        "$NM" --defined-only "${OBJS[@]}" 2>/dev/null | awk '$2=="T" && $3 ~ /^psp_(at|func)_/ {print "void " $3 "(void);"}' | sort -u
        echo "void psp_recomp_register(void) {"
        "$NM" --defined-only "${OBJS[@]}" 2>/dev/null | awk '$2=="T" && $3 ~ /^psp_func_/ {a=substr($3,10); printf "    psp_register(0x%su, %s);\n", a, $3}' | sort -u
        "$NM" --defined-only "${OBJS[@]}" 2>/dev/null | awk '$2=="T" && $3 ~ /^psp_at_/   {a=substr($3,8);  printf "    psp_register_label(0x%su, %s);\n", a, $3}' | sort -u
        echo "}"
    } > "$REG_C"
    "$CC" $CFLAGS $INCS -c "$REG_C" -o "$OBJDIR/register_gen.o" || exit 1
    OBJS+=("$OBJDIR/register_gen.o")
fi

# ---- stubs for units not in this link (subset only) ---------------------
DEF=$("$NM" --defined-only "${OBJS[@]}" 2>/dev/null | awk '{print $3}' | sort -u)
UND=$("$NM" -u "${OBJS[@]}" 2>/dev/null | awk '{print $2}' | sort -u)
MISSING=$(comm -23 <(echo "$UND") <(echo "$DEF") | grep -E '^psp_(func|at)_')
if [ -n "$MISSING" ]; then
    n=$(echo "$MISSING" | wc -l)
    echo "generating $n stubs for symbols outside this link"
    {
        echo "/* generated by build.sh: countable stubs for corpus units not linked */"
        echo '#include "runtime_min.h"'
        for s in $MISSING; do
            case "$s" in
                psp_func_*) a=${s#psp_func_} ;;
                psp_at_*)   a=${s#psp_at_} ;;
            esac
            echo "void $s(void) { psp_stub_hit(0x${a}u); }"
        done
    } > "$OUT/stubs_gen.c"
    "$CC" $CFLAGS $INCS -c "$OUT/stubs_gen.c" -o "$OBJDIR/stubs_gen.o" || exit 1
    OBJS+=("$OBJDIR/stubs_gen.o")
fi

# libmpeg (IPU MPEG decoder): its archive members are LTO objects, so renaming symbols
# with objcopy does not reach the code. memalign/free are wrapped instead (ps2_mpeg.c
# sends libmpeg's allocations to the game's movie ring buffer; the EE heap has no room).
# ---- link ---------------------------------------------------------------
echo "linking ${#OBJS[@]} objects"
LINKLOG="$OUT/link.log"
if ! "$CC" -T"$SDK/ee/startup/linkfile" -Os -o "$OUT/djmax.elf" "${OBJS[@]}" \
        -L"$SDK/ee/lib" -L"$SDK/ports/lib" -lpad -lmc -laudsrv -lpatches -lmpeg -ldma -lz -lkernel -lcglue -lc -lm -lgcc -Wl,--wrap=memalign -Wl,--wrap=free > "$LINKLOG" 2>&1; then
    echo "LINK FAILED"; sed -n '1,40p' "$LINKLOG"; exit 1
fi

ELAPSED=$(( $(date +%s) - START ))
EE_END=$("$NM" "$OUT/djmax.elf" | awk '$3 == "_end" {print $1; exit}')
if [ -z "$EE_END" ] || (( 16#$EE_END > 0x01FE0000 )); then
    echo "ERROR: ELF _end=${EE_END:-missing}; must not exceed 0x01FE0000 (EE stack reserve)"
    exit 1
fi
echo "EE RAM: _end=0x$EE_END (limit 0x01FE0000)"
{
    echo "== djmax.elf ($MODE) =="
    ls -l "$OUT/djmax.elf"
    "$READELF" -h "$OUT/djmax.elf" | sed -n '1,20p'
    echo "-- program headers --"
    "$READELF" -l "$OUT/djmax.elf" | sed -n '1,40p'
    echo "-- size --"
    "$SIZE" "$OUT/djmax.elf"
    echo "-- entry disassembly --"
    "$OBJDUMP" -d "$OUT/djmax.elf" --start-address=0x100000 2>/dev/null | sed -n '1,20p'
    echo "build time: ${ELAPSED}s, objects: ${#OBJS[@]}"
} | tee "$OUT/build-report.txt"

echo "OK -> $OUT/djmax.elf"
