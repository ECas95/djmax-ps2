/* ps2_zlib.c -- the game's zlib 1.2.3 inflate, run by the native zlib (ps2sdk ports).
 *
 * The recompiled inflate was most of the song list's CPU: it unpacks jackets and
 * previews while the list scrolls, and discovery cut inflate_fast into dozens of
 * one-block "functions". These replace the three entry points the game calls:
 *
 *   0x79A58 inflateInit2_(strm, windowBits, version, stream_size)  (inflateInit_ at
 *           0x79BCC calls it, so it is covered too)
 *   0x79EE8 inflate(strm, flush)
 *   0x79C1C inflateEnd(strm)
 *
 * Nothing else reaches zlib's internals (0x7B8C0, the only caller of 0x79D98, has no
 * callers). The guest z_stream (56 bytes, 32-bit) keeps its public fields in sync; its
 * `state` is a token for the native stream. Flush values and return codes are zlib's.
 */
#include "runtime_min.h"

#include <string.h>
#include <zlib.h>

/* Guest z_stream offsets (zlib 1.2.3, ILP32). */
enum { ZS_NEXT_IN = 0, ZS_AVAIL_IN = 4, ZS_TOTAL_IN = 8, ZS_NEXT_OUT = 12, ZS_AVAIL_OUT = 16,
       ZS_TOTAL_OUT = 20, ZS_MSG = 24, ZS_STATE = 28, ZS_DATA_TYPE = 44, ZS_ADLER = 48 };

#define ZSLOTS 6
static struct { uint32_t guest; int used; z_stream zs; } g_z[ZSLOTS];

static int zslot(uint32_t strm) {
    for (int i = 0; i < ZSLOTS; i++) if (g_z[i].used && g_z[i].guest == strm) return i;
    return -1;
}

/* inflateInit2_ */
void psp_func_00079A58(void) {
    const uint32_t strm = psp_cpu.r[PSP_REG_A0] & PSP_ADDR_MASK;
    const int wbits = (int32_t)psp_cpu.r[PSP_REG_A1];
    if (!strm || psp_cpu.r[PSP_REG_A3] != 56u) { psp_cpu.r[PSP_REG_V0] = (uint32_t)Z_VERSION_ERROR; return; }
    int s = zslot(strm);
    if (s >= 0) inflateEnd(&g_z[s].zs);              /* re-init without End: reuse the slot */
    else for (s = 0; s < ZSLOTS && g_z[s].used; s++) { }
    if (s == ZSLOTS) {
        psp_printf("[zlib] out of native streams\n");
        psp_cpu.r[PSP_REG_V0] = (uint32_t)Z_MEM_ERROR;
        return;
    }
    memset(&g_z[s].zs, 0, sizeof g_z[s].zs);
    const int r = inflateInit2(&g_z[s].zs, wbits);
    g_z[s].used = r == Z_OK;
    g_z[s].guest = strm;
    psp_write32(strm + ZS_TOTAL_IN, 0);
    psp_write32(strm + ZS_TOTAL_OUT, 0);
    psp_write32(strm + ZS_MSG, 0);
    psp_write32(strm + ZS_STATE, r == Z_OK ? 0x5A000000u | (uint32_t)s : 0u);
    psp_write32(strm + ZS_ADLER, 1);
    psp_cpu.r[PSP_REG_V0] = (uint32_t)r;
}

/* inflate */
void psp_func_00079EE8(void) {
    const uint32_t strm = psp_cpu.r[PSP_REG_A0] & PSP_ADDR_MASK;
    const int flush = (int32_t)psp_cpu.r[PSP_REG_A1];
    const int s = strm ? zslot(strm) : -1;
    if (s < 0) { psp_cpu.r[PSP_REG_V0] = (uint32_t)Z_STREAM_ERROR; return; }
    z_stream *z = &g_z[s].zs;
    const uint32_t in = psp_read32(strm + ZS_NEXT_IN), ain = psp_read32(strm + ZS_AVAIL_IN);
    const uint32_t out = psp_read32(strm + ZS_NEXT_OUT), aout = psp_read32(strm + ZS_AVAIL_OUT);
    uint8_t *hin = ain ? (uint8_t *)psp_mem_ptr(in, ain) : NULL;
    uint8_t *hout = aout ? (uint8_t *)psp_mem_ptr(out, aout) : NULL;
    if ((ain && !hin) || (aout && !hout)) {
        psp_printf("[zlib] buffer outside guest memory: in=0x%08X+%u out=0x%08X+%u\n", in, ain, out, aout);
        psp_cpu.r[PSP_REG_V0] = (uint32_t)Z_STREAM_ERROR;
        return;
    }
    static uint8_t dummy;
    z->next_in = hin ? hin : &dummy;  z->avail_in = ain;
    z->next_out = hout ? hout : &dummy; z->avail_out = aout;
    const int r = inflate(z, flush);
    const uint32_t used_in = ain - z->avail_in, made = aout - z->avail_out;
    psp_write32(strm + ZS_NEXT_IN, in + used_in);
    psp_write32(strm + ZS_AVAIL_IN, z->avail_in);
    psp_write32(strm + ZS_TOTAL_IN, psp_read32(strm + ZS_TOTAL_IN) + used_in);
    psp_write32(strm + ZS_NEXT_OUT, out + made);
    psp_write32(strm + ZS_AVAIL_OUT, z->avail_out);
    psp_write32(strm + ZS_TOTAL_OUT, psp_read32(strm + ZS_TOTAL_OUT) + made);
    psp_write32(strm + ZS_DATA_TYPE, (uint32_t)z->data_type);
    psp_write32(strm + ZS_ADLER, (uint32_t)z->adler);
    /* msg: a guest string, so the game can print it ("invalid block type", 0xD9D2C). */
    psp_write32(strm + ZS_MSG, z->msg ? 0x000D9D2Cu : 0u);
    if (r < 0 && r != Z_BUF_ERROR) {
        static uint32_t nerr;
        if (nerr++ < 8) psp_printf("[zlib] inflate=%d (%s) strm=0x%08X\n", r, z->msg ? z->msg : "", strm);
    }
    psp_cpu.r[PSP_REG_V0] = (uint32_t)r;
}

/* inflateEnd */
void psp_func_00079C1C(void) {
    const uint32_t strm = psp_cpu.r[PSP_REG_A0] & PSP_ADDR_MASK;
    const int s = strm ? zslot(strm) : -1;
    if (s < 0) { psp_cpu.r[PSP_REG_V0] = (uint32_t)Z_STREAM_ERROR; return; }
    inflateEnd(&g_z[s].zs);
    g_z[s].used = 0;
    psp_write32(strm + ZS_STATE, 0);
    psp_cpu.r[PSP_REG_V0] = Z_OK;
}
