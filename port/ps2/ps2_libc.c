/* ps2_libc.c -- native versions of the game's hot C library routines.
 *
 * The recompiled memcpy moves one guest word per emitted load/store, and every one
 * goes through psp_read32/psp_write32 with their range checks. The song list copies
 * two 116-byte keyframes for each of ~12,000 animation layers per frame (0x3ECC), which
 * costs ~1 s per frame that way. These replace the emitted entry points by address;
 * make_ps2_corpus.ps1 drops the emitted wrappers so these are the ones linked.
 *
 * Contract kept from the originals (read from the MIPS): arguments in a0..a2, v0 = dst.
 * The originals also clobber t0/t1/a1..a3, which callers already treat as scratch.
 */
#include "runtime_min.h"

#include <string.h>

/* The host memcpy. ps2sdk's (weak) copies 16 bytes at a time only when both pointers
 * are 16-aligned and byte by byte otherwise, and host file reads land in guest buffers
 * at any alignment: that byte loop was 15-30% of the EE. Words whatever the source
 * alignment (the EE loads unaligned words with lwl/lwr).
 * no-tree-loop-distribute-patterns: GCC must not turn these loops back into memcpy. */
typedef struct { uint32_t v; } __attribute__((packed, may_alias)) u32_unal;
__attribute__((optimize("no-tree-loop-distribute-patterns")))
void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (n >= 16u) {
        while ((uintptr_t)d & 3u) { *d++ = *s++; n--; }
        uint32_t *dw = (uint32_t *)d;
        if (!((uintptr_t)s & 3u)) {
            const uint32_t *sw = (const uint32_t *)s;
            for (; n >= 16u; n -= 16u, dw += 4, sw += 4) {
                const uint32_t a = sw[0], b = sw[1], c = sw[2], e = sw[3];
                dw[0] = a; dw[1] = b; dw[2] = c; dw[3] = e;
            }
            for (; n >= 4u; n -= 4u) *dw++ = *sw++;
            s = (const uint8_t *)sw;
        } else {
            const u32_unal *su = (const u32_unal *)s;
            for (; n >= 16u; n -= 16u, dw += 4, su += 4) {
                const uint32_t a = su[0].v, b = su[1].v, c = su[2].v, e = su[3].v;
                dw[0] = a; dw[1] = b; dw[2] = c; dw[3] = e;
            }
            for (; n >= 4u; n -= 4u) *dw++ = (su++)->v;
            s = (const uint8_t *)su;
        }
        d = (uint8_t *)dw;
    }
    while (n--) *d++ = *s++;
    return dst;
}

/* Host span for [a, a+n), or NULL if it crosses a window or is unmapped. */
static uint8_t *span(uint32_t a, uint32_t n) {
    return (uint8_t *)psp_mem_ptr(a, n);
}

extern uint32_t g_watch_lo, g_watch_len;   /* TEMP write watch (runtime_min.c) */
static void watch_span(const char *who, uint32_t d, uint32_t n) {
    d &= PSP_ADDR_MASK;
    if (g_watch_len && d < g_watch_lo + g_watch_len && g_watch_lo < d + n)
        psp_printf("[watch] %s dst=0x%08X n=%u ra=0x%08X\n", who, d, n, psp_cpu.r[PSP_REG_RA]);
}

/* 0x9A18C memcpy(dst, src, n): a forward copy, so an overlapping dst > src smears
 * exactly like the original instead of behaving like memmove. */
void psp_func_0009A18C(void) {
    const uint32_t d = psp_cpu.r[PSP_REG_A0], s = psp_cpu.r[PSP_REG_A1], n = psp_cpu.r[PSP_REG_A2];
    watch_span("memcpy", d, n);
    if (((d & PSP_ADDR_MASK) & 0xFF000000u) == 0x04000000u) {   /* PSP VRAM: not mapped here */
        static uint32_t logged;
        if (logged++ < 6u) psp_printf("[vram] memcpy dst=0x%08X src=0x%08X n=%u ra=0x%08X\n", d, s, n, psp_cpu.r[PSP_REG_RA]);
        psp_cpu.r[PSP_REG_V0] = d;
        return;
    }
    uint8_t *pd = n ? span(d, n) : NULL, *ps = n ? span(s, n) : NULL;
    if (pd && ps) {
        if (pd <= ps || pd >= ps + n) memmove(pd, ps, n);
        else for (uint32_t i = 0; i < n; i++) pd[i] = ps[i];
    } else {
        for (uint32_t i = 0; i < n; i++) psp_write8(d + i, psp_read8(s + i));
    }
    psp_cpu.r[PSP_REG_V0] = d;
}

/* 0x9A260 memmove(dst, src, n) */
void psp_func_0009A260(void) {
    const uint32_t d = psp_cpu.r[PSP_REG_A0], s = psp_cpu.r[PSP_REG_A1], n = psp_cpu.r[PSP_REG_A2];
    watch_span("memmove", d, n);
    uint8_t *pd = n ? span(d, n) : NULL, *ps = n ? span(s, n) : NULL;
    if (pd && ps) {
        memmove(pd, ps, n);
    } else if (d <= s) {
        for (uint32_t i = 0; i < n; i++) psp_write8(d + i, psp_read8(s + i));
    } else {
        for (uint32_t i = n; i-- > 0; ) psp_write8(d + i, psp_read8(s + i));
    }
    psp_cpu.r[PSP_REG_V0] = d;
}

/* 0x9A390 memset(dst, byte, n) */
void psp_func_0009A390(void) {
    const uint32_t d = psp_cpu.r[PSP_REG_A0], c = psp_cpu.r[PSP_REG_A1] & 0xFFu, n = psp_cpu.r[PSP_REG_A2];
    watch_span("memset", d, n);
    uint8_t *pd = n ? span(d, n) : NULL;
    if (pd) memset(pd, (int)c, n);
    else for (uint32_t i = 0; i < n; i++) psp_write8(d + i, (uint8_t)c);
    psp_cpu.r[PSP_REG_V0] = d;
}
