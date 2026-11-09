/* ps2_ge.c -- run PSP GE display lists on the PS2 GS.
 *
 * Every GE command is one word: op<<24 | arg. A list runs synchronously up to its
 * stall address, inside sceGeListEnQueue / sceGeListUpdateStallAddr, so the game
 * sees a GE that is always done and DrawSync/ListSync can simply return 0.
 *
 * GE state is mirrored in g_r[256]; a PRIM reads what it needs from there, the GS
 * registers are set once per draw, and vertices go out as A+D register writes.
 * ponytail: A+D costs 48 B per textured vertex, REGLIST halves it if bandwidth bites.
 *
 * Screen: the PSP's 480x272 is scaled to 640x448 in the vertex coordinates. The two
 * PSP framebuffers (fbp 0 and 0x88000) map to two GS frames; textures live above.
 *
 * Host test: tools-src/test_ge_tex.py compiles this file on the PC with GE_HOST_TEST,
 * which replaces only the hardware block just below.
 */
#include "ps2_ge.h"
#include "runtime_min.h"
#ifndef GE_HOST_TEST
#include "ps2_bga.h"
#endif

#include <string.h>
typedef struct { uint32_t v; } __attribute__((packed, may_alias)) u32_unal;

/* ---- GS / GIF / DMA, without a library for what is a dozen registers -------- */
#ifndef GE_HOST_TEST
#include <kernel.h>

#define R64(a) (*(volatile uint64_t *)(a))
#define R32(a) (*(volatile uint32_t *)(a))
#define D2_CHCR     R32(0x1000A000)
#define D2_MADR     R32(0x1000A010)
#define D2_QWC      R32(0x1000A020)

/* Blocks until the GIF has it. FlushCache is whole-cache but the D-cache is 8 KB,
 * so once per kick it costs little next to the transfer. */
static void dma_kick(const void *p, uint32_t qwc) {
    FlushCache(0);
    D2_QWC = qwc;
    D2_MADR = (uint32_t)p;
    D2_CHCR = 0x101u;
    while (D2_CHCR & 0x100u) { }
}

/* COP0 Count: CPU clock, 294.912 MHz, wraps every 14.5 s. */
static inline uint32_t ge_cycles(void) {
    uint32_t c;
    __asm__ volatile("mfc0 %0, $9" : "=r"(c));
    return c;
}
#endif

#define GS_PMODE    R64(0x12000000)
#define GS_SMODE2   R64(0x12000020)
#define GS_DISPFB1  R64(0x12000070)
#define GS_DISPLAY1 R64(0x12000080)
#define GS_BGCOLOR  R64(0x120000E0)

enum { G_PRIM = 0x00, G_RGBAQ = 0x01, G_UV = 0x03, G_XYZ2 = 0x05, G_TEX0 = 0x06, G_CLAMP = 0x08,
       G_TEX1 = 0x14, G_XYOFFSET = 0x18, G_PRMODECONT = 0x1A, G_TEXA = 0x3B, G_TEXFLUSH = 0x3F,
       G_SCISSOR = 0x40, G_ALPHA = 0x42, G_DIMX = 0x44, G_DTHE = 0x45, G_COLCLAMP = 0x46, G_TEST = 0x47, G_FRAME = 0x4C,
       G_ZBUF = 0x4E, G_BITBLTBUF = 0x50, G_TRXPOS = 0x51, G_TRXREG = 0x52, G_TRXDIR = 0x53 };

#define SCR_W    640
#define SCR_H    448
#define PSP_W    480
#define PSP_H    272
#define GS_OFS   2048
#define GS_PAGES 512u                /* 4 MB in 8 KB pages */

/* 1: two PSMCT16 frames with dither. Frees 1.1 MB for textures; costs 5 bits per
 * channel on everything drawn, where the PSP game renders in 8888. */
#ifndef GE_FB16
#define GE_FB16 1
#endif
#if GE_FB16
#define FB_PSM   2u
#define FB_PAGES 70u                 /* 640*448*2 / 8192 */
#else
#define FB_PSM   0u
#define FB_PAGES 140u                /* 640*448*4 / 8192 */
#endif

#define PKT_QW   5000
static uint64_t g_pkt[PKT_QW * 2] __attribute__((aligned(64)));
static uint32_t g_pn;                /* qwords used, including the open tag */

static void gif_flush(void) {
    if (!g_pn) return;
    /* The first qword is the A+D GIFTAG: patch its count now it is known. */
    g_pkt[0] = (uint64_t)(g_pn - 1u) | (1ull << 15) | (1ull << 60);
    g_pkt[1] = 0xEull;
    dma_kick(g_pkt, g_pn);
    g_pn = 0;
}

static inline void ad(uint32_t reg, uint64_t val) {
    if (g_pn == 0) g_pn = 1;
    g_pkt[g_pn * 2u] = val;
    g_pkt[g_pn * 2u + 1u] = reg;
    if (++g_pn == PKT_QW) gif_flush();
}

/* ---- GE state ------------------------------------------------------------- */
#define GE_LISTS     8
#define GE_CALLS     32
#define GE_RUN_MAX   2000000u     /* a list that never ends must not hang the game */
#define GE_ADDR_MASK 0x0FFFFFFFu

typedef struct {
    int      used;
    uint32_t pc, stall;           /* stall 0: run to the end */
    uint32_t stack[GE_CALLS];     /* return pc, saved offset */
    uint32_t saved_ofs[GE_CALLS];
    int      sp;
} ge_list;

static ge_list  g_list[GE_LISTS];
static uint32_t g_r[256];         /* last argument of each command */
static uint32_t g_base, g_offset, g_vaddr, g_iaddr;
static float    g_world[12], g_view[12], g_proj[16], g_texmtx[12];
static uint32_t g_wi, g_vi, g_pi, g_ti;
static uint32_t g_fb_cur = 0xFFFFFFFFu;

/* The GE sends a float as its top 24 bits. */
static inline float f24(uint32_t a) {
    union { uint32_t u; float f; } c;
    c.u = a << 8;
    return c.f;
}

/* ---- statistics, in the log every few seconds ----------------------------- */
static struct {
    uint32_t lists, cmds, prims, verts, xfers, bad, texmiss, texbad;
    uint32_t vt_seen[8], fmt_seen;
    uint32_t texchg, up_bytes, up_cyc, evict, blbad;
    uint32_t cte, ste, lop, rejected, notex;   /* GE features the GS translation drops */
    uint32_t wset;                              /* pool pages the frame's textures need */
} S;
static uint32_t g_frames, g_frames_drawn;
/* TEMP frame dump: every prim and block transfer of one frame, on request (R3 or
 * host:gedump.txt), to see what a screen draws that the GS translation loses. */
static int g_dump_req, g_dump;
static uint32_t g_dump_i;
void ps2_ge_dump_request(void) { g_dump_req = 1; }
/* The same counters added up since the last report: one frame's snapshot can mislead. */
static struct { uint32_t frames, texmiss, up_bytes, evict, wset_max; uint64_t up_cyc, cyc; } W;
static uint32_t g_cyc_last;

/* Size in bytes of one vertex: each member is aligned to its own component size. */
uint32_t ps2_ge_vertex_size(uint32_t vt) {
    static const uint8_t tc[4]  = {0, 2, 4, 8}, tca[4] = {1, 1, 2, 4};
    static const uint8_t col[8] = {0, 0, 0, 0, 2, 2, 2, 4}, cola[8] = {1, 1, 1, 1, 2, 2, 2, 4};
    static const uint8_t nrm[4] = {0, 3, 6, 12}, nrma[4] = {1, 1, 2, 4};
    static const uint8_t pos[4] = {0, 3, 6, 12}, posa[4] = {1, 1, 2, 4};
    static const uint8_t wgt[4] = {0, 1, 2, 4};
    const uint32_t t = vt & 3u, c = (vt >> 2) & 7u, n = (vt >> 5) & 3u, p = (vt >> 7) & 3u;
    const uint32_t w = (vt >> 9) & 3u, wc = ((vt >> 14) & 7u) + 1u;
    uint32_t sz = wgt[w] * wc, a = w ? wgt[w] : 1u;
#define ADD(s, al) do { if (s) { sz = (sz + (al) - 1u) & ~((al) - 1u); sz += (s); \
                                 if ((al) > a) a = (al); } } while (0)
    ADD(tc[t], tca[t]);
    ADD(col[c], cola[c]);
    ADD(nrm[n], nrma[n]);
    ADD(pos[p], posa[p]);
#undef ADD
    return (sz + a - 1u) & ~(a - 1u);
}

/* ---- textures ---------------------------------------------------------------
 * GS memory: the two frames, then one CLUT slot per cache entry, then a pool of
 * 8 KB pages. A texture takes contiguous pages and stays there until evicted.
 * GE format -> GS format: 5650/5551 to CT16, CLUT4/8 to T4/T8 + CT32 CLUT, the rest
 * (4444 too: its gradient alpha does not fit CT16's one bit) to CT32.
 * ponytail: whole pages, so a 16x16 texture costs 8 KB; sub-page blocks if tiny ones add up.
 * ponytail: one CLUT slot per entry, used or not (64 KB in all); an allocator if that hurts.
 * ponytail: DXT always CT32, 4x its PSP size; opaque art could be CT16 if covers crowd the pool.
 */
enum { PSM_CT32 = 0x00, PSM_CT16 = 0x02, PSM_T8 = 0x13, PSM_T4 = 0x14 };

#define TC_MAX     64
#define CLUT_BASE  (FB_PAGES * 2u * 32u)         /* in 256-byte blocks; 4 per entry */
#define CLUT_PAGES (TC_MAX * 4u / 32u)
#define POOL_BASE  (FB_PAGES * 2u + CLUT_PAGES)  /* in pages */
#define POOL_PAGES (GS_PAGES - POOL_BASE)
_Static_assert(POOL_PAGES >= 128u, "a 512x512 CT32 texture must fit");

typedef struct { uint8_t psm, bpp, pw, ph; } gs_fmt;   /* ph, pw: texels per page */
static const gs_fmt g_gsfmt[4] = {
    {PSM_CT32, 32, 64, 32}, {PSM_CT16, 16, 64, 64}, {PSM_T8, 8, 128, 64}, {PSM_T4, 4, 128, 128}
};
static const uint8_t g_fmt2gs[11] = {1, 1, 0, 0, 3, 2, 0, 0, 0, 0, 0};
/* Bits per texel in guest memory for GE formats 0..7 (8.. are DXT). */
static const uint8_t g_bpp[8] = {16, 16, 16, 32, 4, 8, 16, 32};

typedef struct {
    uint32_t addr, key, hash, last;   /* last: g_tick when last bound */
    uint16_t pg, npg;                 /* pool pages; npg 0 is a free entry */
    uint64_t tex0;                    /* TBP0, TBW, PSM and the CLUT fields */
} tc_ent;
static tc_ent   g_tc[TC_MAX];
static uint8_t  g_pg[POOL_PAGES];     /* entry index + 1 owning each page, 0 free */
static uint32_t g_tick = 1, g_tick_frame;    /* binds so far, and at the start of this frame */
static uint32_t g_pal[256];           /* the current palette, resolved and in GS alpha */
static uint64_t g_tex0_cur = ~0ull;

/* Eviction order, smallest first. Textures not used this frame go oldest first.
 * Those used this frame only go when nothing else can, and then the most recent
 * first: a frame redraws the same textures in the same order, so what it has just
 * drawn is what it needs last, and plain LRU would miss on every bind of such a cycle. */
static uint32_t tc_cost(const tc_ent *e) {
    return e->last <= g_tick_frame ? e->last : 0x80000000u + (g_tick - e->last);
}

static void tc_evict(uint32_t i) {
    tc_ent *e = &g_tc[i];
    memset(&g_pg[e->pg], 0, e->npg);
    e->npg = 0;
    S.evict++;
}

/* Free n contiguous pool pages, evicting the cheapest window. Returns its first page.
 * ponytail: O(pool * n) per miss, a free list if misses stop being rare. */
static uint32_t tc_take(uint32_t n) {
    uint32_t best = 0, best_cost = ~0u, best_used = ~0u;
    for (uint32_t s = 0; s + n <= POOL_PAGES; s++) {
        uint32_t cost = 0, used = 0;
        for (uint32_t i = s; i < s + n && cost <= best_cost; i++) {
            if (!g_pg[i]) continue;
            const uint32_t c = tc_cost(&g_tc[g_pg[i] - 1u]);
            if (c > cost) cost = c;
            used++;
        }
        if (cost < best_cost || (cost == best_cost && used < best_used)) {
            best = s; best_cost = cost; best_used = used;
        }
    }
    for (uint32_t i = best; i < best + n; i++)
        if (g_pg[i]) tc_evict(g_pg[i] - 1u);
    return best;
}

/* A free cache entry, evicting the cheapest one if there is none. */
static uint32_t tc_slot(void) {
    uint32_t best = 0;
    for (uint32_t i = 0; i < TC_MAX; i++) {
        if (!g_tc[i].npg) return i;
        if (tc_cost(&g_tc[i]) < tc_cost(&g_tc[best])) best = i;
    }
    tc_evict(best);
    return best;
}

static inline uint32_t rgba32(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    /* GS: texture alpha 0x80 is 1.0. */
    return r | (g << 8) | (b << 16) | (((a + 1u) >> 1) << 24);
}

static uint32_t px_decode(uint32_t fmt, uint32_t v) {
    switch (fmt) {
    case 0: return rgba32((v & 31u) * 255u / 31u, ((v >> 5) & 63u) * 255u / 63u,
                          ((v >> 11) & 31u) * 255u / 31u, 255u);
    case 1: return rgba32((v & 31u) * 255u / 31u, ((v >> 5) & 31u) * 255u / 31u,
                          ((v >> 10) & 31u) * 255u / 31u, (v >> 15) ? 255u : 0u);
    case 2: return rgba32((v & 15u) * 17u, ((v >> 4) & 15u) * 17u,
                          ((v >> 8) & 15u) * 17u, ((v >> 12) & 15u) * 17u);
    default: return rgba32(v & 255u, (v >> 8) & 255u, (v >> 16) & 255u, v >> 24);
    }
}

/* g_pal[i] = the colour the GE shows for index i (shift, mask and offset of 0xC5). */
/* GE colour test (CTE 0x27, CTST 0xD8, CREF 0xD9, CMSK 0xDA): the GS has none. Games use
 * it as a colour key (magenta texels never drawn), so it is applied to the texels at
 * upload, keyed ones get alpha 0, and the draw adds an alpha test that drops them.
 * ponytail: tests the texel, not the final fragment; same thing for key-coloured art. */
static struct { int on; uint32_t ref, mask, eq, tcc; } g_ck;

static void ck_state(void) {
    const uint32_t f = g_r[0xD8] & 3u;
    g_ck.on = (g_r[0x27] & 1u) && (f == 2u || f == 3u);
    g_ck.ref = g_r[0xD9] & 0xFFFFFFu;
    g_ck.mask = g_r[0xDA] & 0xFFFFFFu;
    g_ck.eq = f == 2u;
    g_ck.tcc = (g_r[0xC9] >> 8) & 1u;
}
static uint32_t ck_sig(void) {
    return g_ck.on ? (g_ck.ref * 2654435761u) ^ (g_ck.mask * 40503u) ^ (g_ck.eq << 30) ^ (g_ck.tcc << 31) ^ 0x5A5Au : 0u;
}
static inline int ck_drop(uint32_t rgb) {
    return (((rgb ^ g_ck.ref) & g_ck.mask) == 0u) != (int)g_ck.eq;
}
/* GS CT32 texels (alpha 0x80 = 1.0). Texture alpha unused by the GE (TCC RGB) becomes 1.0
 * on the kept ones, since the draw then reads it. */
static void ck_ct32(uint32_t *t, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t rgb = t[i] & 0xFFFFFFu;
        if (ck_drop(rgb)) t[i] = 0u;     /* black: bilinear edges darken instead of going pink */
        else if (!g_ck.tcc) t[i] = rgb | 0x80000000u;
    }
}
static void ck_ct16(uint16_t *t, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t v = t[i], r = v & 31u, g = (v >> 5) & 31u, b = (v >> 10) & 31u;
        const uint32_t rgb = ((r << 3) | (r >> 2)) | ((g << 3) | (g >> 2)) << 8 | ((b << 3) | (b >> 2)) << 16;
        if (ck_drop(rgb)) t[i] = 0u;
        else if (!g_ck.tcc) t[i] = (uint16_t)(v | 0x8000u);
    }
}

static void clut_build(uint32_t n, uint32_t shift, uint32_t mask) {
    const uint32_t m = g_r[0xC5], cfmt = m & 3u, start = ((m >> 16) & 31u) << 4;
    const uint32_t addr = (g_r[0xB0] & 0xFFFFFFu) | ((g_r[0xB1] & 0xFF0000u) << 8);
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t idx = ((i >> shift) & mask) | start;
        g_pal[i] = cfmt == 3u ? px_decode(3u, psp_read32(addr + idx * 4u))
                              : px_decode(cfmt, psp_read16(addr + idx * 2u));
    }
}

/* Open an image transfer. Rows then go out through g_pkt as they are made. */
static void img_open(uint32_t dbp, uint32_t dbw, uint32_t psm, uint32_t w, uint32_t h) {
    ad(G_BITBLTBUF, ((uint64_t)dbp << 32) | ((uint64_t)dbw << 48) | ((uint64_t)psm << 56));
    ad(G_TRXPOS, 0);
    ad(G_TRXREG, (uint64_t)w | ((uint64_t)h << 32));
    ad(G_TRXDIR, 0);
    gif_flush();
}

/* Send qw qwords already written at g_pkt[2], under an IMAGE tag. */
static void img_send(uint32_t qw, int last) {
    g_pkt[0] = (uint64_t)qw | (last ? (1ull << 15) : 0ull) | (2ull << 58);
    g_pkt[1] = 0;
    dma_kick(g_pkt, qw + 1u);
}

/* g_pal into GS memory, CT32. 256 colours in CSM1 sit with bits 3 and 4 of the
 * index swapped; 16 colours are a plain 8x2 rectangle. */
static void clut_upload(uint32_t cbp, uint32_t n) {
    uint32_t *o = (uint32_t *)&g_pkt[2];
    img_open(cbp, 1u, PSM_CT32, n == 16u ? 8u : 16u, n == 16u ? 2u : 16u);
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t j = n == 16u ? i : (i & 0xE7u) | ((i & 8u) << 1) | ((i & 16u) >> 1);
        o[j] = g_pal[i];
    }
    img_send(n / 4u, 1);
    S.up_bytes += n * 4u;
}

/* One row of guest texels, unswizzled, at the start of d. bytes is a multiple of 16. */
static void row_gather(uint8_t *d, const uint8_t *src, uint32_t swz, uint32_t pitch,
                       uint32_t y, uint32_t bytes) {
    if (!swz) { memcpy(d, src + (size_t)pitch * y, bytes); return; }
    /* PSP swizzle: within each 8 rows, 16-byte blocks stored column by column. */
    const uint32_t blocks = pitch / 16u ? pitch / 16u : 1u;
    const uint8_t *p = src + (size_t)(y / 8u) * blocks * 128u + (y % 8u) * 16u;
    for (uint32_t i = 0; i < bytes; i += 16u, p += 128u) {   /* one 16-byte block row, inline */
        const u32_unal *q = (const u32_unal *)p;
        uint32_t *o = (uint32_t *)(d + i);
        o[0] = q[0].v; o[1] = q[1].v; o[2] = q[2].v; o[3] = q[3].v;
    }
}

/* The gathered row into its GS format, in place. 16 -> 32 bits runs backwards so
 * it never overwrites what it has not read yet. */
static void row_conv(uint8_t *d, uint32_t fmt, uint32_t w) {
    uint16_t *s16 = (uint16_t *)d;
    uint32_t *d32 = (uint32_t *)d;
    const uint32_t sh = (g_r[0xC5] >> 2) & 31u;
    uint32_t i;
    switch (fmt) {
    case 0:   /* 5650 -> CT16, loses the low green bit */
        for (i = 0; i < w; i++) s16[i] = (uint16_t)((s16[i] & 0x1Fu) | ((s16[i] >> 1) & 0x7FE0u) | 0x8000u);
        break;
    case 2:   /* 4444 -> CT32 */
        for (i = w; i-- > 0u; ) {
            const uint32_t v = s16[i];
            const uint32_t c = ((v & 0x0Fu) | ((v & 0xF0u) << 4) | ((v & 0xF00u) << 8) | ((v & 0xF000u) << 12)) * 17u;
            d32[i] = (c & 0xFFFFFFu) | (((c >> 24) + 1u) >> 1) << 24;
        }
        break;
    case 3:   /* 8888 -> CT32: only the alpha scale */
        for (i = 0; i < w; i++) d32[i] = (d32[i] & 0xFFFFFFu) | (((d32[i] >> 24) + 1u) >> 1) << 24;
        break;
    case 6: for (i = w; i-- > 0u; ) d32[i] = g_pal[(s16[i] >> sh) & 255u]; break;
    case 7: for (i = 0; i < w; i++) d32[i] = g_pal[(d32[i] >> sh) & 255u]; break;
    default: break;   /* 5551 is CT16 as it is; 4 and 5 are T4 and T8 as they are */
    }
}

/* ---- DXT ---- */
/* v*255/31 and v*255/63: what the game's art was checked against (gim2png.py). */
static const uint8_t g_x5[32] = {0,8,16,24,32,41,49,57,65,74,82,90,98,106,115,123,131,139,148,156,
                                 164,172,180,189,197,205,213,222,230,238,246,255};
static const uint8_t g_x6[64] = {0,4,8,12,16,20,24,28,32,36,40,44,48,52,56,60,64,68,72,76,80,85,89,93,
                                 97,101,105,109,113,117,121,125,129,133,137,141,145,149,153,157,161,165,
                                 170,174,178,182,186,190,194,198,202,206,210,214,218,222,226,230,234,238,
                                 242,246,250,255};
/* Exact over the ranges used here (x <= 766, 1785, 1275); checked by brute force. */
#define DIV3(x) (((x) * 43691u) >> 17)
#define DIV7(x) (((x) * 9363u) >> 16)
#define DIV5(x) (((x) * 13108u) >> 16)

/* One 4x4 block to RGBA8 (alpha 255 = opaque), rows pitch texels apart. The PSP layout
 * is rotated against PC DDS: colour block first with its indices before its endpoints,
 * then the alpha block, whose (DXT5) indices also come before its endpoints.
 *   +0 u32 colour indices  +4 u16 c0  +6 u16 c1
 *   +8 DXT3: 4 u16 rows of 4-bit alpha   DXT5: 6 bytes of 3-bit indices, +14 a0, +15 a1 */
static void ge_dxt_block(const uint8_t *b, uint32_t fmt, uint32_t *out, uint32_t pitch) {
    const uint32_t c0 = b[4] | ((uint32_t)b[5] << 8), c1 = b[6] | ((uint32_t)b[7] << 8);
    const uint32_t r0 = g_x5[c0 >> 11], g0 = g_x6[(c0 >> 5) & 63u], b0 = g_x5[c0 & 31u];
    const uint32_t r1 = g_x5[c1 >> 11], g1 = g_x6[(c1 >> 5) & 63u], b1 = g_x5[c1 & 31u];
    uint32_t col[4], bits = b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    col[0] = r0 | (g0 << 8) | (b0 << 16) | 0xFF000000u;
    col[1] = r1 | (g1 << 8) | (b1 << 16) | 0xFF000000u;
    if (c0 > c1) {
        col[2] = DIV3(2u * r0 + r1 + 1u) | (DIV3(2u * g0 + g1 + 1u) << 8) | (DIV3(2u * b0 + b1 + 1u) << 16) | 0xFF000000u;
        col[3] = DIV3(r0 + 2u * r1 + 1u) | (DIV3(g0 + 2u * g1 + 1u) << 8) | (DIV3(b0 + 2u * b1 + 1u) << 16) | 0xFF000000u;
    } else {
        col[2] = ((r0 + r1) >> 1) | (((g0 + g1) >> 1) << 8) | (((b0 + b1) >> 1) << 16) | 0xFF000000u;
        col[3] = 0u;
    }
    uint32_t *row = out;
    for (uint32_t y = 0; y < 4u; y++, row += pitch)
        for (uint32_t x = 0; x < 4u; x++, bits >>= 2) row[x] = col[bits & 3u];

    if (fmt == 9u) {
        row = out;
        for (uint32_t y = 0; y < 4u; y++, row += pitch) {
            uint32_t v = b[8u + y * 2u] | ((uint32_t)b[9u + y * 2u] << 8);
            for (uint32_t x = 0; x < 4u; x++, v >>= 4)
                row[x] = (row[x] & 0xFFFFFFu) | (((v & 15u) * 17u) << 24);
        }
    } else if (fmt == 10u) {
        const uint32_t a0 = b[14], a1 = b[15];
        uint32_t at[8];
        at[0] = a0; at[1] = a1;
        if (a0 > a1) {
            for (uint32_t i = 2; i < 8u; i++) at[i] = DIV7((8u - i) * a0 + (i - 1u) * a1);
        } else {
            for (uint32_t i = 2; i < 6u; i++) at[i] = DIV5((6u - i) * a0 + (i - 1u) * a1);
            at[6] = 0u; at[7] = 255u;
        }
        row = out;
        for (uint32_t y = 0; y < 4u; y++, row += pitch) {
            uint32_t v = y < 2u ? b[8] | ((uint32_t)b[9] << 8) | ((uint32_t)b[10] << 16)
                                : b[11] | ((uint32_t)b[12] << 8) | ((uint32_t)b[13] << 16);
            if (y & 1u) v >>= 12;
            for (uint32_t x = 0; x < 4u; x++, v >>= 3)
                row[x] = (row[x] & 0xFFFFFFu) | (at[v & 7u] << 24);
        }
    }
}

/* Rows y..y+n of a DXT texture, CT32, straight into the outgoing packet. A block
 * may spill up to 3 rows past n: the caller guarantees whole blocks fit. */
static void dxt_rows(uint8_t *d, const uint8_t *src, uint32_t fmt, uint32_t pitch, uint32_t y,
                     uint32_t n, uint32_t w, uint32_t row_bytes) {
    const uint32_t bsize = fmt == 8u ? 8u : 16u;
    for (uint32_t r = 0; r < n; r += 4u) {
        const uint8_t *b = src + (size_t)((y + r) / 4u) * pitch;
        uint32_t *o = (uint32_t *)(d + (size_t)r * row_bytes);
        for (uint32_t x = 0; x < w; x += 4u, b += bsize) ge_dxt_block(b, fmt, o + x, row_bytes / 4u);
    }
    for (uint32_t r = 0; r < n; r++) row_conv(d + (size_t)r * row_bytes, 3u, w);
}

/* Send a texture to GS memory, one DMA per packetful of rows. Rows are whole qwords
 * (uw is w rounded up to that), so a chunk boundary can never split a texel. */
static void tex_upload(const uint8_t *src, uint32_t fmt, uint32_t swz, uint32_t pitch, const gs_fmt *o,
                       uint32_t tbp, uint32_t tbw, uint32_t w, uint32_t uw, uint32_t h) {
    const uint32_t row_bytes = uw * o->bpp / 8u, row_qw = row_bytes / 16u;
    uint32_t per = (PKT_QW - 1u) / row_qw;
    uint32_t gather = 0;
    if (fmt >= 8u) per &= ~3u;
    else           gather = ((w * g_bpp[fmt] + 127u) / 128u) * 16u;

    img_open(tbp, tbw, o->psm, uw, h);
    for (uint32_t y = 0; y < h; ) {
        const uint32_t n = h - y < per ? h - y : per;
        uint8_t *d = (uint8_t *)&g_pkt[2];
        if (fmt >= 8u) {
            dxt_rows(d, src, fmt, pitch, y, n, w, row_bytes);
        } else {
            for (uint32_t r = 0; r < n; r++, d += row_bytes) {
                row_gather(d, src, swz, pitch, y + r, gather);
                row_conv(d, fmt, w);
            }
        }
        if (g_ck.on) {
            uint8_t *c = (uint8_t *)&g_pkt[2];
            if (o->psm == 0x00u)      ck_ct32((uint32_t *)c, n * uw);
            else if (o->psm == 0x02u) ck_ct16((uint16_t *)c, n * uw);
        }
        y += n;
        img_send(n * row_qw, y == h);
    }
    S.up_bytes += row_bytes * h;
}

/* Cheap content hash: a texture slot reused by the game must not show stale data.
 * One word from each of 128 equal blocks, at a different place in each block: a fixed
 * step is a multiple of the row pitch in a power-of-two texture, so every sample landed
 * in column 0 -- transparent in any text atlas, and the 8B screen showed the title's
 * texture in place of its own. ponytail: still a sample; hash it all if a change slips. */
static uint32_t tex_hash(const uint8_t *p, uint32_t bytes) {
    uint32_t h = 2166136261u;
    if (bytes < 4u) return h;
    const uint32_t n = bytes / 4u < 128u ? bytes / 4u : 128u;
    const uint32_t block = bytes / n & ~3u;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t off = i * block + (block > 4u ? (i * 2654435761u) % (block / 4u) * 4u : 0u);
        h = (h ^ ((const u32_unal *)(p + off))->v) * 16777619u;
    }
    return h;
}

/* The cache entry of the current texture, uploaded if needed. NULL if it cannot be used. */
static const tc_ent *tex_bind(void) {
    const uint32_t fmt = g_r[0xC3] & 15u;
    const uint32_t addr = (g_r[0xA0] & 0xFFFFFFu) | ((g_r[0xA8] & 0xFF0000u) << 8);
    const uint32_t stride = g_r[0xA8] & 0xFFFFu;
    const uint32_t swz = g_r[0xC2] & 1u;
    const uint32_t lw = g_r[0xB8] & 15u, lh = (g_r[0xB8] >> 8) & 15u;
    S.fmt_seen |= 1u << fmt;
    /* 1024 high (lh 10) is past the PSP but fine for the GS: the song title sheets
     * (tools-src/titles.py) are 256x1024. */
    if (fmt > 10u || lw > 9u || lh > 10u || !stride) { S.texbad++; return 0; }
    const uint32_t w = 1u << lw, h = 1u << lh;

    /* Bytes per source row, and how far this texture reaches in guest memory. */
    uint32_t pitch, bytes;
    if (fmt >= 8u) {
        const uint32_t bsize = fmt == 8u ? 8u : 16u;
        const uint32_t reach = ((stride > w ? stride : w) + 3u) / 4u * bsize;
        pitch = (stride + 3u) / 4u * bsize;
        bytes = reach * ((h + 3u) / 4u);
    } else {
        const uint32_t reach = (stride > w ? stride : w) * g_bpp[fmt] / 8u;
        pitch = stride * g_bpp[fmt] / 8u;
        bytes = swz ? (reach / 16u ? reach / 16u : 1u) * 128u * ((h + 7u) / 8u) : reach * h;
    }
    const uint8_t *src = (const uint8_t *)psp_mem_ptr(addr, bytes ? bytes : 1u);
    if (!src) { S.texbad++; return 0; }

    uint32_t bga_hash = 0;
#ifndef GE_HOST_TEST
    if (fmt == 5u && w == 256u && h == 256u && stride == 256u && swz && (g_r[0xC5] & 3u) == 3u) {
        uint32_t ca = (g_r[0xB0] & 0xFFFFFFu) | ((g_r[0xB1] & 0xFF0000u) << 8);
        uint8_t *pal = psp_mem_ptr(ca, 1024);
        if (pal) bga_hash = ps2_bga_texture((uint8_t *)src, pal);
    }
#endif

    const uint32_t key = fmt | (swz << 4) | (lw << 5) | (lh << 9) | (stride << 13);
    ck_state();
    uint32_t hash = tex_hash(src, bytes) ^ ck_sig() ^ (bga_hash * 2654435761u);
    if (fmt >= 4u && fmt <= 7u) {
        const uint32_t m = g_r[0xC5], esize = (m & 3u) == 3u ? 4u : 2u;
        const uint32_t ca = ((g_r[0xB0] & 0xFFFFFFu) | ((g_r[0xB1] & 0xFF0000u) << 8)) + (((m >> 16) & 31u) << 4) * esize;
        const uint32_t cbytes = (((m >> 8) & 255u) + 1u) * esize;
        const uint8_t *c = (const uint8_t *)psp_mem_ptr(ca, cbytes);
        if (c) hash ^= tex_hash(c, cbytes) ^ m;
    }

    g_tick++;
    int changed = 0;
    for (uint32_t i = 0; i < TC_MAX; i++) {
        tc_ent *e = &g_tc[i];
        if (!e->npg || e->addr != addr || e->key != key) continue;
        if (e->hash == hash) { if (e->last <= g_tick_frame) S.wset += e->npg; e->last = g_tick; return e; }
        changed = 1;   /* left for LRU to reclaim: it may be the same pixels under a second palette */
    }
    S.texchg += (uint32_t)changed;

    const gs_fmt *o = &g_gsfmt[g_fmt2gs[fmt]];
    const uint32_t minw = 128u / o->bpp;           /* one qword of texels */
    const uint32_t uw = w < minw ? minw : w;
    const uint32_t wpages = (uw + o->pw - 1u) / o->pw;
    const uint32_t tbw = wpages * (o->pw / 64u);
    const uint32_t npg = wpages * ((h + o->ph - 1u) / o->ph);

    S.wset += npg;
    const uint32_t slot = tc_slot();
    const uint32_t pg = tc_take(npg);
    memset(&g_pg[pg], (int)slot + 1, npg);

    const uint32_t tbp = (POOL_BASE + pg) * 32u, cbp = CLUT_BASE + slot * 4u;
    tc_ent *e = &g_tc[slot];
    e->addr = addr; e->key = key; e->hash = hash; e->last = g_tick;
    e->pg = (uint16_t)pg; e->npg = (uint16_t)npg;
    e->tex0 = (uint64_t)tbp | ((uint64_t)tbw << 14) | ((uint64_t)o->psm << 20);
    S.texmiss++;

    const uint32_t t0 = ge_cycles();
    if (fmt == 4u || fmt == 5u) {
        const uint32_t m = g_r[0xC5], n = fmt == 4u ? 16u : 256u;
        clut_build(n, (m >> 2) & 31u, (m >> 8) & 255u);
        if (g_ck.on) ck_ct32(g_pal, n);
        clut_upload(cbp, n);
        /* CPSM CT32, CSM1, CSA 0, CLD 1: reload the palette whenever TEX0 is written. */
        e->tex0 |= ((uint64_t)cbp << 37) | (1ull << 61);
    } else if (fmt == 6u || fmt == 7u) {
        clut_build(256u, 0u, (g_r[0xC5] >> 8) & 255u);
    }
    tex_upload(src, fmt, swz, pitch, o, tbp, tbw, w, uw, h);
    ad(G_TEXFLUSH, 0);
    g_tex0_cur = ~0ull;        /* same TEX0 may now mean other pixels and another palette */
    S.up_cyc += ge_cycles() - t0;
    return e;
}

/* ---- GS draw state -------------------------------------------------------- */
static uint32_t g_tex_w, g_tex_h;
static uint32_t g_sc_a, g_sc_b;

/* GS blend is ((A-B)*C >> 7) + D, with A, B, D from Cs/Cd/0 and C from As/Ad/FIX. */
enum { BL_CS = 0, BL_CD = 1, BL_0 = 2, BL_AS = 0, BL_FIX = 2 };
#define GS_BLEND(a, b, c, d, fix) ((uint64_t)(a) | ((uint64_t)(b) << 2) | ((uint64_t)(c) << 4) \
                                   | ((uint64_t)(d) << 6) | ((uint64_t)(fix) << 32))

/* GE (src, dst, equation) to the GS form. Factors: 2 SA, 3 1-SA, 10 FIX. Colour factors
 * and dest alpha have no GS form (and PSP dest alpha is stencil, which is not emulated):
 * those land on the old nearest guess and show up as blbad in the log. */
static uint64_t gs_blend(void) {
    const uint32_t m = g_r[0xDF], src = m & 15u, dst = (m >> 4) & 15u, eq = (m >> 8) & 7u;
    const uint32_t fa = g_r[0xE0] & 0xFFFFFFu, fb = g_r[0xE1] & 0xFFFFFFu;
    const uint32_t ka = ((fa & 255u) + 1u) >> 1;     /* GS FIX: 0x80 is 1.0 */
    const int sfix = src == 10u, dfix = dst == 10u;
    const int b1 = dfix && fb == 0xFFFFFFu, b0 = dfix && fb == 0u;

    if (eq == 0u) {
        if (src == 2u && dst == 3u) return GS_BLEND(BL_CS, BL_CD, BL_AS, BL_CD, 0);
        if (src == 3u && dst == 2u) return GS_BLEND(BL_CD, BL_CS, BL_AS, BL_CS, 0);
        if (src == 2u && b1)        return GS_BLEND(BL_CS, BL_0, BL_AS, BL_CD, 0);
        if (src == 2u && b0)        return GS_BLEND(BL_CS, BL_0, BL_AS, BL_0, 0);
        if (sfix && b1)             return GS_BLEND(BL_CS, BL_0, BL_FIX, BL_CD, ka);
        if (sfix && b0)             return GS_BLEND(BL_CS, BL_0, BL_FIX, BL_0, ka);
        if (sfix && dfix && (fa & 255u) + (fb & 255u) == 255u)
                                    return GS_BLEND(BL_CS, BL_CD, BL_FIX, BL_CD, ka);
        if (sfix && !fa && dst == 3u) return GS_BLEND(BL_0, BL_CD, BL_AS, BL_CD, 0);
        if (sfix && !fa && dst == 2u) return GS_BLEND(BL_CD, BL_0, BL_AS, BL_0, 0);
        /* "Screen", Cs + Cd*(1-Cs): the GS cannot scale by a colour, so additive, which
         * it matches for dark sources and saturates like it for bright ones. The lane
         * press beam (0x01A) is drawn this way. */
        if (sfix && dst == 1u)      return GS_BLEND(BL_CS, BL_0, BL_FIX, BL_CD, ka);
        if (src == 2u && dst == 1u) return GS_BLEND(BL_CS, BL_0, BL_AS, BL_CD, 0);
    } else if (eq == 1u) {          /* Cs*A - Cd*B */
        if (sfix && fa == 0xFFFFFFu && b1) return GS_BLEND(BL_CS, BL_CD, BL_FIX, BL_0, 128u);
    } else if (eq == 2u) {          /* Cd*B - Cs*A */
        if (src == 2u && b1)        return GS_BLEND(BL_0, BL_CS, BL_AS, BL_CD, 0);
        if (sfix && b1)             return GS_BLEND(BL_0, BL_CS, BL_FIX, BL_CD, ka);
    }
    S.blbad = 0x1000u | m;
    return dfix ? GS_BLEND(BL_CS, BL_0, BL_AS, BL_CD, 0) : GS_BLEND(BL_CS, BL_CD, BL_AS, BL_CD, 0);
}

/* GE scissor (inclusive, PSP pixels) into SCISSOR_1 with the screen scale. Register
 * values 0 and 0 mean the game has not set it: the full screen from init stays. */
static void gs_set_scissor(void) {
    const uint32_t a = g_r[0xD4], b = g_r[0xD5];
    if (a == g_sc_a && b == g_sc_b) return;
    g_sc_a = a; g_sc_b = b;
    const uint32_t x0 = (a & 0x3FFu) * SCR_W / PSP_W, y0 = ((a >> 10) & 0x3FFu) * SCR_H / PSP_H;
    uint32_t x1 = ((b & 0x3FFu) + 1u) * SCR_W / PSP_W, y1 = (((b >> 10) & 0x3FFu) + 1u) * SCR_H / PSP_H;
    if (x1 > SCR_W) x1 = SCR_W;
    if (y1 > SCR_H) y1 = SCR_H;
    ad(G_SCISSOR, (uint64_t)x0 | ((uint64_t)(x1 - 1u) << 16) | ((uint64_t)y0 << 32) | ((uint64_t)(y1 - 1u) << 48));
}

static void gs_set_draw_state(int textured, int clearmode) {
    uint64_t test = 0x30000ull;       /* ZTE on, ZTST ALWAYS: no depth buffer yet */

    if (!clearmode && (g_r[0x22] & 1u)) {
        static const uint8_t ge2gs[8] = {0, 1, 4, 7, 2, 3, 6, 5};
        const uint32_t m = g_r[0xDB];
        uint32_t aref = (((m >> 8) & 255u) + 1u) >> 1;
        /* Colour key on: the PSP tests the filtered colour against the key under a
         * mask (C0C0C0 here), so mostly-key edge pixels go too; a 50% alpha cut does
         * the same to the keyed texels' bilinear fringe. */
        if (textured && (g_r[0x27] & 1u) && (g_r[0xD8] & 3u) >= 2u && ((m & 7u) == 6u || (m & 7u) == 7u) && aref < 0x40u)
            aref = 0x40u;
        test |= 1ull | ((uint64_t)ge2gs[m & 7u] << 1) | ((uint64_t)aref << 4);
    } else if (!clearmode && textured && (g_r[0x27] & 1u) && (g_r[0xD8] & 3u) >= 2u) {
        test |= 1ull | (6ull << 1) | (0x40ull << 4);   /* colour key: ATST GREATER, AREF 50% */
        S.cte++;
    }
    ad(G_TEST, test);

    /* Blending off: Cs. */
    ad(G_ALPHA, !clearmode && (g_r[0x21] & 1u) ? gs_blend() : GS_BLEND(BL_CS, BL_0, BL_FIX, BL_0, 128u));

    if (textured) {
        const tc_ent *e = tex_bind();
        if (!e) { g_tex_w = 0; return; }
        const uint32_t lw = g_r[0xB8] & 15u, lh = (g_r[0xB8] >> 8) & 15u;
        g_tex_w = 1u << lw;
        g_tex_h = 1u << lh;
        const uint32_t tf = g_r[0xC9];
        const uint64_t tfx = (tf & 7u) == 3u ? 1u : 0u;
        const uint64_t tcc = ((tf >> 8) & 1u) | (g_ck.on ? 1u : 0u);
        const uint64_t tex0 = e->tex0 | ((uint64_t)lw << 26) | ((uint64_t)lh << 30) | (tcc << 34) | (tfx << 35);
        /* Skipping a repeat also skips reloading the palette into the CLUT buffer. */
        if (tex0 != g_tex0_cur) { ad(G_TEX0, tex0); g_tex0_cur = tex0; }
        const uint32_t filt = g_r[0xC6];
        ad(G_TEX1, ((uint64_t)((filt >> 8) & 1u) << 5) | ((uint64_t)(filt & 1u) << 6));
        const uint32_t wrap = g_r[0xC7];
        ad(G_CLAMP, (uint64_t)(wrap & 1u) | ((uint64_t)((wrap >> 8) & 1u) << 2));
    } else {
        g_tex_w = 0;
    }
}

static void gs_set_frame(uint32_t fbp) {
    if (fbp == g_fb_cur) return;
    g_fb_cur = fbp;
    const uint32_t page = fbp ? FB_PAGES : 0u;
    ad(G_FRAME, (uint64_t)page | ((uint64_t)(SCR_W / 64) << 16) | ((uint64_t)FB_PSM << 24));
}

/* ---- vertices --------------------------------------------------------------- */
typedef struct { float x, y, z, u, v; uint32_t col; } vtx;

static inline void mul43(const float *m, const float *in, float *out) {
    out[0] = in[0] * m[0] + in[1] * m[3] + in[2] * m[6] + m[9];
    out[1] = in[0] * m[1] + in[1] * m[4] + in[2] * m[7] + m[10];
    out[2] = in[0] * m[2] + in[1] * m[5] + in[2] * m[8] + m[11];
}

static inline uint32_t col16(uint32_t fmt, uint32_t v) {
    /* Vertex colour as RGBA8, same layouts as textures. */
    switch (fmt) {
    case 4: return ((v & 31u) * 255u / 31u) | (((v >> 5) & 63u) * 255u / 63u) << 8
                 | (((v >> 11) & 31u) * 255u / 31u) << 16 | 0xFF000000u;
    case 5: return ((v & 31u) * 255u / 31u) | (((v >> 5) & 31u) * 255u / 31u) << 8
                 | (((v >> 10) & 31u) * 255u / 31u) << 16 | ((v >> 15) ? 0xFF000000u : 0u);
    default: return (v & 15u) * 17u | ((v >> 4) & 15u) * 17u << 8
                  | ((v >> 8) & 15u) * 17u << 16 | ((v >> 12) & 15u) * 17u << 24;
    }
}

static int vtx_read(uint32_t a, uint32_t vt, vtx *o) {
    const uint32_t t = vt & 3u, c = (vt >> 2) & 7u, n = (vt >> 5) & 3u, p = (vt >> 7) & 3u;
    const uint32_t w = (vt >> 9) & 3u, wc = ((vt >> 14) & 7u) + 1u;
    const int through = (vt >> 23) & 1u;
    static const uint8_t wsz[4] = {0, 1, 2, 4};
    uint32_t q = a + wsz[w] * (w ? wc : 0u);
#define AL(al) (q = (q + (al) - 1u) & ~((al) - 1u))
    o->u = o->v = 0.0f;
    switch (t) {
    case 1: o->u = (float)psp_read8(q); o->v = (float)psp_read8(q + 1u); q += 2u;
            if (!through) { o->u /= 128.0f; o->v /= 128.0f; } break;
    case 2: AL(2u); o->u = (float)psp_read16(q); o->v = (float)psp_read16(q + 2u); q += 4u;
            if (!through) { o->u /= 32768.0f; o->v /= 32768.0f; } break;
    case 3: AL(4u); o->u = psp_read_f32(q); o->v = psp_read_f32(q + 4u); q += 8u; break;
    default: break;
    }
    if (c >= 4u && c <= 6u) { AL(2u); o->col = col16(c, psp_read16(q)); q += 2u; }
    else if (c == 7u)       { AL(4u); o->col = psp_read32(q); q += 4u; }
    else {
        /* No per-vertex colour: the material colour. */
        o->col = (g_r[0x55] & 0xFFFFFFu) | ((g_r[0x58] & 255u) << 24);
    }
    if (n == 1u) q += 3u; else if (n == 2u) { AL(2u); q += 6u; } else if (n == 3u) { AL(4u); q += 12u; }
    float in[3] = {0, 0, 0};
    switch (p) {
    case 1: in[0] = (float)(int8_t)psp_read8(q); in[1] = (float)(int8_t)psp_read8(q + 1u);
            in[2] = (float)(int8_t)psp_read8(q + 2u); break;
    case 2: AL(2u); in[0] = (float)(int16_t)psp_read16(q); in[1] = (float)(int16_t)psp_read16(q + 2u);
            in[2] = (float)(through ? (uint16_t)psp_read16(q + 4u) : (int16_t)psp_read16(q + 4u)); break;
    case 3: AL(4u); in[0] = psp_read_f32(q); in[1] = psp_read_f32(q + 4u); in[2] = psp_read_f32(q + 8u); break;
    default: return 0;
    }
#undef AL
    if (through) {
        o->x = in[0]; o->y = in[1]; o->z = in[2];
        return 1;
    }
    if (p == 1u) { in[0] /= 128.0f; in[1] /= 128.0f; in[2] /= 128.0f; }
    if (p == 2u) { in[0] /= 32768.0f; in[1] /= 32768.0f; in[2] /= 32768.0f; }

    /* world -> view -> projection -> viewport */
    float wv[3], vv[3];
    mul43(g_world, in, wv);
    mul43(g_view, wv, vv);
    const float cw = vv[0] * g_proj[3] + vv[1] * g_proj[7] + vv[2] * g_proj[11] + g_proj[15];
    if (cw <= 0.00001f) return 0;
    const float cx = vv[0] * g_proj[0] + vv[1] * g_proj[4] + vv[2] * g_proj[8] + g_proj[12];
    const float cy = vv[0] * g_proj[1] + vv[1] * g_proj[5] + vv[2] * g_proj[9] + g_proj[13];
    const float cz = vv[0] * g_proj[2] + vv[1] * g_proj[6] + vv[2] * g_proj[10] + g_proj[14];
    o->x = cx / cw * f24(g_r[0x42]) + f24(g_r[0x45]) - (float)(g_r[0x4C] & 0xFFFFu) / 16.0f;
    o->y = cy / cw * f24(g_r[0x43]) + f24(g_r[0x46]) - (float)(g_r[0x4D] & 0xFFFFu) / 16.0f;
    o->z = cz / cw * f24(g_r[0x44]) + f24(g_r[0x47]);
    /* ponytail: affine UV. All GE geometry seen so far is flat sprites; use STQ for depth. */
    o->u = o->u * f24(g_r[0x48]) + f24(g_r[0x4A]);
    o->v = o->v * f24(g_r[0x49]) + f24(g_r[0x4B]);
    return 1;
}

/* Whole texture periods taken off this prim's UVs (in texels): a repeating texture
 * scrolled by moving its UVs (Fever's "COMBO x2" strips run at u = -8.2) wraps the
 * same from any period, and the GS takes only 0..1023 texels, so start near 0. */
static int g_uoff, g_voff;

static inline void vtx_emit(const vtx *v, int textured, int through) {
    const uint32_t c = v->col;
    /* GS alpha 128 is 1.0 (blend and test), so alpha always halves. RGB 128 is 1.0 only
     * where the GS modulates a texture by it; untextured, RGB is written as is. */
    const uint32_t sh = textured ? 1u : 0u, rnd = sh;
    const uint64_t rgba = ((c & 255u) + rnd) >> sh | (uint64_t)((((c >> 8) & 255u) + rnd) >> sh) << 8
                        | (uint64_t)((((c >> 16) & 255u) + rnd) >> sh) << 16
                        | (uint64_t)(((c >> 24) + 1u) >> 1) << 24 | (0x3F800000ull << 32);
    ad(G_RGBAQ, rgba);
    if (textured) {
        const float us = through ? 1.0f : (float)g_tex_w, vs = through ? 1.0f : (float)g_tex_h;
        int u = (int)((v->u * us - (float)g_uoff) * 16.0f), t = (int)((v->v * vs - (float)g_voff) * 16.0f);
        if (u < 0) u = 0;
        if (t < 0) t = 0;
        ad(G_UV, (uint64_t)(u & 0x3FFF) | ((uint64_t)(t & 0x3FFF) << 16));
    }
    int x = (int)(v->x * (float)SCR_W / (float)PSP_W * 16.0f) + (GS_OFS - SCR_W / 2) * 16;
    int y = (int)(v->y * (float)SCR_H / (float)PSP_H * 16.0f) + (GS_OFS - SCR_H / 2) * 16;
    /* The GS coordinate space ends at 4096: past it the value wraps across the screen. */
    if (x < 0) x = 0; else if (x > 0xFFFF) x = 0xFFFF;
    if (y < 0) y = 0; else if (y > 0xFFFF) y = 0xFFFF;
    ad(G_XYZ2, (uint64_t)x | ((uint64_t)y << 16));
}

static void ge_prim(uint32_t type, uint32_t count) {
    const uint32_t vt = g_r[0x12];
    const uint32_t itype = (vt >> 11) & 3u;
    const uint32_t vsz = ps2_ge_vertex_size(vt);
    const int clearmode = g_r[0xD3] & 1u;
    const int through = (vt >> 23) & 1u;
    const int want_tex = !clearmode && (g_r[0x1E] & 1u) && (vt & 3u);

    S.prims++;
    S.verts += count;
    for (int i = 0; i < 8; i++) {
        if (S.vt_seen[i] == vt) break;
        if (!S.vt_seen[i]) { S.vt_seen[i] = vt; break; }
    }
    if (type > 6u || !vsz) { S.rejected++; return; }
    if (g_r[0x24] & 1u) S.ste++;
    if (g_r[0x28] & 1u) S.lop++;

    if (g_dump) {
        psp_printf("[dump] p%u va=%08X type=%u n=%u vt=%06X clear=%u fb=%06X/%u tme=%u tex=%06X%02X fmt=%u %ux%u tf=%06X abe=%u bl=%06X fa=%06X fb=%06X ate=%u at=%06X cte=%u ct=%u/%06X/%06X zte=%u mat=%06X/%02X sc=%06X-%06X lop=%u ste=%u\n",
                   g_dump_i++, g_vaddr, type, count, vt, clearmode, g_r[0x9C] & 0xFFFFFFu, g_r[0x9D] & 0xFFFFu,
                   g_r[0x1E] & 1u, g_r[0xA0] & 0xFFFFFFu, (g_r[0xA8] >> 16) & 0xFFu, g_r[0xC3] & 15u,
                   1u << (g_r[0xB8] & 15u), 1u << ((g_r[0xB8] >> 8) & 15u), g_r[0xC9] & 0xFFFFFFu,
                   g_r[0x21] & 1u, g_r[0xDF] & 0xFFFFFFu, g_r[0xE0] & 0xFFFFFFu, g_r[0xE1] & 0xFFFFFFu,
                   g_r[0x22] & 1u, g_r[0xDB] & 0xFFFFFFu, g_r[0x27] & 1u, g_r[0xD8] & 3u, g_r[0xD9] & 0xFFFFFFu,
                   g_r[0xDA] & 0xFFFFFFu, g_r[0x23] & 1u, g_r[0x55] & 0xFFFFFFu, g_r[0x58] & 0xFFu,
                   g_r[0xD4] & 0xFFFFFFu, g_r[0xD5] & 0xFFFFFFu, g_r[0x28] & 1u, g_r[0x24] & 1u);
    }
    gs_set_frame(g_r[0x9C] & 0xFFFFFFu);
    gs_set_scissor();
    gs_set_draw_state(want_tex, clearmode);
    const int textured = want_tex && g_tex_w;
    if (want_tex && !g_tex_w) S.notex++;
    const uint32_t abe = !clearmode && (g_r[0x21] & 1u);
    ad(G_PRIM, (uint64_t)type | (1ull << 3) /* gouraud */ | ((uint64_t)textured << 4)
             | ((uint64_t)abe << 6) | (1ull << 8) /* UV */);

    /* Strips and fans must not lose a vertex: a rejected one repeats the previous. */
    vtx v;
    int have = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = i;
        if (itype == 1u)      idx = psp_read8(g_iaddr + i);
        else if (itype == 2u) idx = psp_read16(g_iaddr + i * 2u);
        else if (itype == 3u) idx = psp_read32(g_iaddr + i * 4u);
        if (vtx_read(g_vaddr + idx * vsz, vt, &v) && !have) {
            have = 1;
            g_uoff = g_voff = 0;
            if (textured) {   /* REPEAT axes (wrap bit clear): shift by whole periods */
                const float us = through ? 1.0f : (float)g_tex_w, vs = through ? 1.0f : (float)g_tex_h;
                const float pu = v.u * us / (float)g_tex_w, pv = v.v * vs / (float)g_tex_h;
                /* One period of margin (when it fits) for a later vertex below this one. */
                if (!(g_r[0xC7] & 1u))        { int k = (int)pu; if ((float)k > pu) k--; g_uoff = (k - (g_tex_w <= 256u)) * (int)g_tex_w; }
                if (!((g_r[0xC7] >> 8) & 1u)) { int k = (int)pv; if ((float)k > pv) k--; g_voff = (k - (g_tex_h <= 256u)) * (int)g_tex_h; }
            }
        }
        if (have) vtx_emit(&v, textured, through);
        if (g_dump && (i == 0u || i + 1u == count)) {
            psp_printf("[dump]    v%u x=%d y=%d z=%d u=%d v=%d col=%08X ok=%d\n", i, (int)v.x, (int)v.y,
                       (int)v.z, (int)(v.u * 100.0f), (int)(v.v * 100.0f), v.col, have);
        }
    }
}

/* ---- block transfer (0xEA) ---------------------------------------------------
 * RAM -> framebuffer is how the movie player (sceMpeg) shows its decoded frames. The GS
 * frame is the PSP screen scaled, so a copy cannot be a BITBLT: the source goes through
 * the texture cache and is drawn as a sprite at the destination, scaled like any prim.
 * Other transfers (VRAM to VRAM, RAM to RAM) are not modelled. */
static uint32_t g_xfer_other;

static uint32_t log2ceil(uint32_t v) { uint32_t l = 0; while ((1u << l) < v && l < 10u) l++; return l; }

/* Draw w x h pixels of a RAM image (stride in pixels, 8888 or the framebuffer's 16-bit
 * format) at (dx, dy) of the PSP framebuffer fbp, from source pixel (sx, sy). */
/* A sprite of texture e (log2 size lw x lh) from texel (sx, sy), w x h, at (dx, dy) of
 * the PSP framebuffer fbp; scaled like every prim, RGB only. */
static void blit_draw(const tc_ent *e, uint32_t lw, uint32_t lh, uint32_t fbp, uint32_t sx, uint32_t sy,
                      uint32_t w, uint32_t h, uint32_t dx, uint32_t dy) {
    gs_set_frame(fbp);
    ad(G_SCISSOR, (uint64_t)(SCR_W - 1) << 16 | (uint64_t)(SCR_H - 1) << 48);
    g_sc_a = g_sc_b = ~0u;                           /* the next prim sets its own */
    ad(G_TEST, 0x30000ull);
    ad(G_TEX0, e->tex0 | ((uint64_t)lw << 26) | ((uint64_t)lh << 30) | (1ull << 35));   /* RGB, DECAL */
    g_tex0_cur = ~0ull;
    ad(G_TEX1, (1ull << 5) | (1ull << 6));           /* bilinear: the copy is scaled */
    ad(G_CLAMP, 1ull | (1ull << 2));
    ad(G_PRIM, 6ull | (1ull << 4) | (1ull << 8));    /* sprite, textured, UV */
    g_uoff = g_voff = 0;
    g_tex_w = 1u << lw; g_tex_h = 1u << lh;
    vtx v;
    memset(&v, 0, sizeof v);
    v.col = 0xFFFFFFFFu;
    v.x = (float)dx; v.y = (float)dy; v.u = (float)sx; v.v = (float)sy;
    vtx_emit(&v, 1, 1);
    v.x = (float)(dx + w); v.y = (float)(dy + h); v.u = (float)(sx + w); v.v = (float)(sy + h);
    vtx_emit(&v, 1, 1);
    S.prims++;
}

static void blit_ram(uint32_t src, uint32_t sw, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h,
                     uint32_t fbp, uint32_t dx, uint32_t dy, int bpp32) {
    /* The texture registers say "this RAM image" for tex_bind, then go back. */
    static const uint8_t regs[] = { 0xA0, 0xA8, 0xB8, 0xC2, 0xC3 };
    uint32_t saved[sizeof regs];
    for (unsigned i = 0; i < sizeof regs; i++) saved[i] = g_r[regs[i]];
    const uint32_t lw = log2ceil(sx + w), lh = log2ceil(sy + h);
    g_r[0xA0] = src & 0xFFFFFFu;
    g_r[0xA8] = ((src >> 24) & 0xFFu) << 16 | sw;
    g_r[0xB8] = lw | lh << 8;
    g_r[0xC2] = 0;                                   /* linear, not swizzled */
    g_r[0xC3] = bpp32 ? 3u : (g_r[0xD2] & 3u);       /* 8888, or the framebuffer's 16-bit format */
    const tc_ent *e = tex_bind();
    for (unsigned i = 0; i < sizeof regs; i++) g_r[regs[i]] = saved[i];
    if (!e) return;
    blit_draw(e, lw, lh, fbp, sx, sy, w, h, dx, dy);
}

static void ge_transfer(uint32_t arg) {
    const uint32_t src = (g_r[0xB0] & 0xFFFFFFu) | ((g_r[0xB1] & 0xFF0000u) << 8);
    const uint32_t sw  = g_r[0xB1] & 0x7FFu;
    const uint32_t dst = (g_r[0xB2] & 0xFFFFFFu) | ((g_r[0xB3] & 0xFF0000u) << 8);
    const uint32_t sx = g_r[0xEB] & 0x3FFu, sy = (g_r[0xEB] >> 10) & 0x3FFu;
    const uint32_t dx = g_r[0xEC] & 0x3FFu, dy = (g_r[0xEC] >> 10) & 0x3FFu;
    const uint32_t w = (g_r[0xEE] & 0x3FFu) + 1u, h = ((g_r[0xEE] >> 10) & 0x3FFu) + 1u;
    if ((dst & 0x0F000000u) != 0x04000000u || (src & 0x0F000000u) != 0x08000000u || !sw) {
        if (g_xfer_other++ < 4u)
            psp_printf("[ge] transfer not modelled: src=0x%08X dst=0x%08X %ux%u\n", src, dst, w, h);
        return;
    }
    blit_ram(src, sw, sx, sy, w, h, dst & 0x1FFFFFu, dx, dy, arg & 1u);
}

/* sceDmacMemcpy into VRAM: the movie player copies each decoded frame (RAM, 8888, stride
 * 512) over a whole framebuffer, 30 times a second. The texture cache path (hash + per-texel
 * conversion of 512x512) cost 16 ms a frame, so: one fixed cache entry, the 512 x rows
 * image sent to it by a DMA chain straight from guest memory, then the scaled sprite.
 * ponytail: 8888 at stride 512, the only display format this game sets (SetFrameBuf 3). */
#define MOVIE_KEY  0xFFFFFFF0u
static int g_movie_slot = -1;

void ps2_ge_vram_copy(uint32_t dst, uint32_t src, uint32_t size) {
    const uint32_t stride = 512u;
    uint32_t rows = size / (stride * 4u);
    if (rows > PSP_H) rows = PSP_H;
    const uint8_t *img = rows ? (const uint8_t *)psp_mem_ptr(src, stride * 4u * rows) : NULL;
    if (!img) return;
    if ((uintptr_t)img & 15u) {                      /* DMA needs 16 bytes: the slow path */
        blit_ram(src, stride, 0, 0, PSP_W, rows, dst & 0x1FFFFFu, 0, 0, 1);
        gif_flush();
        return;
    }
    const uint32_t npg = (stride / 64u) * ((PSP_H + 31u) / 32u);   /* CT32 pages: 64 x 32 texels */
    tc_ent *e = g_movie_slot >= 0 ? &g_tc[g_movie_slot] : NULL;
    if (!e || !e->npg || e->addr != MOVIE_KEY) {
        const uint32_t slot = tc_slot();
        const uint32_t pg = tc_take(npg);
        memset(&g_pg[pg], (int)slot + 1, npg);
        e = &g_tc[slot];
        e->addr = MOVIE_KEY; e->key = MOVIE_KEY; e->hash = 0;
        e->pg = (uint16_t)pg; e->npg = (uint16_t)npg;
        e->tex0 = (uint64_t)((POOL_BASE + pg) * 32u) | ((uint64_t)(stride / 64u) << 14) | ((uint64_t)PSM_CT32 << 20);
        g_movie_slot = (int)slot;
    }
    e->last = g_tick;

    const uint32_t qw = stride * 4u * rows / 16u;
    img_open((uint32_t)(e->tex0 & 0x3FFFu), stride / 64u, PSM_CT32, stride, rows);
    /* rows copied into the GIF packet and sent in pieces, as tex_upload does */
    const uint32_t row_qw = stride * 4u / 16u, per = (PKT_QW - 1u) / row_qw;
    for (uint32_t y = 0; y < rows; ) {
        const uint32_t n = rows - y < per ? rows - y : per;
        memcpy(&g_pkt[2], img + (size_t)y * stride * 4u, (size_t)n * stride * 4u);
        y += n;
        img_send(n * row_qw, y == rows);
    }
    ad(G_TEXFLUSH, 0);
    blit_draw(e, 9u, 9u, dst & 0x1FFFFFu, 0, 0, PSP_W, rows, 0, 0);
    gif_flush();
}

/* ---- the list walker ------------------------------------------------------ */
/* Runs until the stall, FINISH/END, or a list that points outside memory. */
static void ge_run(ge_list *l) {
    uint32_t budget = GE_RUN_MAX;
    while (l->used) {
        if (budget-- == 0u) { S.bad++; l->used = 0; return; }
        if (l->stall && (l->pc & GE_ADDR_MASK) == (l->stall & GE_ADDR_MASK)) return;
        if (!psp_mem_ptr(l->pc, 4)) { S.bad++; l->used = 0; return; }

        const uint32_t w = psp_read32(l->pc);
        const uint32_t op = w >> 24, arg = w & 0xFFFFFFu;
        const uint32_t rel = (((g_base & 0x00FF0000u) << 8) | arg) + g_offset;
        l->pc += 4u;
        S.cmds++;
        g_r[op] = arg;

        switch (op) {
        case 0x01: g_vaddr = rel; break;
        case 0x02: g_iaddr = rel; break;
        case 0x04: {
            const uint32_t count = arg & 0xFFFFu;
            ge_prim(arg >> 16, count);
            /* The GE leaves VADDR/IADDR just past what it read; lists rely on it. */
            const uint32_t it = (g_r[0x12] >> 11) & 3u;
            if (it) g_iaddr += (it == 3u ? 4u : it) * count;
            else    g_vaddr += ps2_ge_vertex_size(g_r[0x12]) * count;
            break;
        }
        case 0x08: l->pc = rel & ~3u; break;                         /* JUMP */
        case 0x0A:                                                   /* CALL */
            if (l->sp < GE_CALLS) {
                l->saved_ofs[l->sp] = g_offset;
                l->stack[l->sp++] = l->pc;
            }
            l->pc = rel & ~3u;
            break;
        case 0x0B:                                                   /* RET */
            if (l->sp > 0) {
                l->pc = l->stack[--l->sp];
                g_offset = l->saved_ofs[l->sp];
            }
            break;
        case 0x0C: l->used = 0; return;                              /* END */
        case 0x0E: l->pc += 4u; break;                               /* SIGNAL + its END */
        case 0x0F: l->used = 0; return;                              /* FINISH */
        case 0x10: g_base = arg; break;
        case 0x13: g_offset = arg << 8; break;
        case 0x14: g_offset = l->pc - 4u; break;                     /* ORIGIN */
        case 0x3A: g_wi = arg & 15u; break;
        case 0x3B: if (g_wi < 12u) g_world[g_wi++] = f24(arg); break;
        case 0x3C: g_vi = arg & 15u; break;
        case 0x3D: if (g_vi < 12u) g_view[g_vi++] = f24(arg); break;
        case 0x3E: g_pi = arg & 15u; break;
        case 0x3F: if (g_pi < 16u) g_proj[g_pi++] = f24(arg); break;
        case 0x40: g_ti = arg & 15u; break;
        case 0x41: if (g_ti < 12u) g_texmtx[g_ti++] = f24(arg); break;
        case 0xEA:
            S.xfers++;
            if (g_dump)
                psp_printf("[dump] xfer src=%06X/%06X dst=%06X/%06X spos=%06X dpos=%06X size=%06X bpp32=%u\n",
                           g_r[0xB0] & 0xFFFFFFu, g_r[0xB1] & 0xFFFFFFu, g_r[0xB2] & 0xFFFFFFu, g_r[0xB3] & 0xFFFFFFu,
                           g_r[0xEB] & 0xFFFFFFu, g_r[0xEC] & 0xFFFFFFu, g_r[0xEE] & 0xFFFFFFu, arg & 1u);
            ge_transfer(arg);
            break;
        default: break;
        }
    }
}

uint32_t g_ge_cyc;   /* TEMP [slow] */
uint32_t ps2_ge_enqueue(uint32_t list, uint32_t stall) {
    for (uint32_t id = 0; id < GE_LISTS; id++) {
        ge_list *l = &g_list[id];
        if (l->used) continue;
        l->used = 1;
        l->pc = list;
        l->stall = stall;
        l->sp = 0;
        S.lists++;
        const uint32_t t0 = ge_cycles();
        ge_run(l);
        gif_flush();
        g_ge_cyc += ge_cycles() - t0;
        return id;
    }
    return 0x80000021u;      /* queue full */
}

void ps2_ge_update_stall(uint32_t id, uint32_t stall) {
    if (id >= GE_LISTS || !g_list[id].used) return;
    g_list[id].stall = stall;
    const uint32_t t0 = ge_cycles();
    ge_run(&g_list[id]);
    gif_flush();
    g_ge_cyc += ge_cycles() - t0;
}

/* ---- display ---------------------------------------------------------------- */
/* sceDisplaySetFrameBuf(.., sync): sync 1 (NEXTFRAME) latches at the next vblank, as
 * the PSP does: a flip written mid-scanout tears and can be overwritten before it is
 * ever shown. sync 0 (IMMEDIATE) shows it now: the game's second flip path (0xACCEC)
 * uses it and starts drawing into the other buffer at once, so latching it left that
 * buffer on screen while it was being drawn (sprites flickering on those screens). */
static volatile uint64_t g_dispfb_next;
static volatile int g_dispfb_pending;

static int ge_vblank(int cause) {
    (void)cause;
    if (g_dispfb_pending) { GS_DISPFB1 = g_dispfb_next; g_dispfb_pending = 0; }
    ExitHandler();
    return 0;
}

void ps2_ge_init(void) {
    AddIntcHandler(2, ge_vblank, 0);
    EnableIntc(2);
    SetGsCrt(1, 2, 0);                       /* interlaced, NTSC, field: 448 lines in memory */
    GS_PMODE = 0xFF25ull;                    /* read circuit 1 only, ALP=0xFF, no BG mix */
    GS_SMODE2 = 0x1ull;                      /* interlace, FFMD=field */
    GS_DISPFB1 = ((uint64_t)(SCR_W / 64) << 9) | ((uint64_t)FB_PSM << 15);
    GS_DISPLAY1 = (uint64_t)652 | ((uint64_t)50 << 12) | (3ull << 23)
                | ((uint64_t)(SCR_W * 4 - 1) << 32) | ((uint64_t)(SCR_H - 1) << 44);
    GS_BGCOLOR = 0;

    ad(G_FRAME, ((uint64_t)(SCR_W / 64) << 16) | ((uint64_t)FB_PSM << 24));
#if GE_FB16
    ad(G_DIMX, 0x6071243571603524ull);       /* the usual 4x4 ordered dither */
    ad(G_DTHE, 1);
#endif
    ad(G_ZBUF, 1ull << 32);                  /* ZMSK: nothing writes depth */
    ad(G_XYOFFSET, 0);
    ad(G_SCISSOR, (uint64_t)(GS_OFS - SCR_W / 2) | ((uint64_t)(GS_OFS + SCR_W / 2 - 1) << 16)
                | ((uint64_t)(GS_OFS - SCR_H / 2) << 32) | ((uint64_t)(GS_OFS + SCR_H / 2 - 1) << 48));
    ad(G_PRMODECONT, 1);
    ad(G_COLCLAMP, 1);
    ad(G_TEXA, 0x80ull << 32);               /* CT16: A bit 0 is alpha 0, A bit 1 is 0x80 (1.0) */
    gif_flush();

    /* XYOFFSET centres on GS_OFS-based coordinates: vertices already include it. */
    ad(G_SCISSOR, (uint64_t)(SCR_W - 1) << 16 | (uint64_t)(SCR_H - 1) << 48);
    ad(G_XYOFFSET, (uint64_t)((GS_OFS - SCR_W / 2) * 16) | ((uint64_t)((GS_OFS - SCR_H / 2) * 16) << 32));
    gif_flush();
    g_fb_cur = 0;
    g_cyc_last = ge_cycles();
}

static void report_frame(void) {
    uint32_t tc = 0, pg = 0;
    for (uint32_t i = 0; i < TC_MAX; i++) if (g_tc[i].npg) { tc++; pg += g_tc[i].npg; }
    psp_printf("[gef %u] lists=%u cmds=%u prims=%u verts=%u xfers=%u bad=%u texup=%u texbad=%u texchg=%u upkb=%u evict=%u fmts=0x%X tc=%u tcpg=%u/%u blbad=0x%X guest_bad=%llu misses=%llu hle=%llu\n",
               g_frames, S.lists, S.cmds, S.prims, S.verts, S.xfers, S.bad, S.texmiss, S.texbad,
               S.texchg, S.up_bytes / 1024u, S.evict, S.fmt_seen, tc, pg, POOL_PAGES, S.blbad,
               (unsigned long long)psp_mem_bad_access,
               (unsigned long long)psp_dispatch_misses(), (unsigned long long)psp_hle_total_calls());
    /* fps: Count-based, emulated time. 30 fps is 9830 kcycles per frame. */
    const uint32_t cyc = (uint32_t)(W.cyc / W.frames);
    psp_printf("[gef]  avg/frame over %u: fps=%u kcyc=%u upkcyc=%u texup=%u upkb=%u evict=%u\n",
               W.frames, cyc ? 294912000u / cyc : 0u, cyc / 1000u, (uint32_t)(W.up_cyc / W.frames / 1000u),
               W.texmiss / W.frames, W.up_bytes / W.frames / 1024u, W.evict / W.frames);
    psp_printf("[gef]  wset_max=%u pages (pool %u)\n", W.wset_max, POOL_PAGES);
    psp_printf("[gef]  dropped: cte=%u ste=%u lop=%u rejected=%u notex=%u\n", S.cte, S.ste, S.lop, S.rejected, S.notex);
    psp_printf("[gef]  vtypes:");
    for (int i = 0; i < 8 && S.vt_seen[i]; i++) psp_printf(" 0x%06X", S.vt_seen[i]);
    psp_printf("\n");
    memset(&W, 0, sizeof W);
}

/* ---- "SAVING" indicator (ps2_savedata.c) -------------------------------------
 * The PSP shows a system dialog while it writes a save; here there is none, so a box with
 * SAVING and moving dots goes on every frame shown while one is in progress (top right,
 * in PSP coordinates, scaled like the game). 5x7 glyphs, 1.5x1.5 PSP pixels per dot. */
static int g_osd_frames, g_osd_tick;
void ps2_ge_osd_saving(int frames) { g_osd_frames = frames; }

static const uint8_t g_osd_font[6][7] = {
    { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E },   /* S */
    { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },   /* A */
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 },   /* V */
    { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E },   /* I */
    { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 },   /* N */
    { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F },   /* G */
};

static void osd_rect(float x0, float y0, float x1, float y1, uint32_t col) {
    vtx v;
    memset(&v, 0, sizeof v);
    v.col = col;
    v.x = x0; v.y = y0; vtx_emit(&v, 0, 1);
    v.x = x1; v.y = y1; vtx_emit(&v, 0, 1);
}

static void osd_draw(uint32_t fbp) {
    static const uint8_t word[6] = { 0, 1, 2, 3, 4, 5 };   /* S A V I N G */
    const float px = 1.5f, x0 = 472.0f - 45.0f * px, y0 = 4.0f + 3.0f * px;   /* everything in px */
    gs_set_frame(fbp);
    ad(G_SCISSOR, (uint64_t)(SCR_W - 1) << 16 | (uint64_t)(SCR_H - 1) << 48);
    g_sc_a = g_sc_b = ~0u;
    ad(G_TEST, 0x30000ull);
    ad(G_ALPHA, GS_BLEND(BL_CS, BL_CD, BL_AS, BL_CD, 0));
    ad(G_PRIM, 6ull | (1ull << 6));                  /* sprite, blended, untextured */
    osd_rect(x0 - 4.0f * px, y0 - 3.0f * px, x0 + 45.0f * px, y0 + 10.0f * px, 0xC0000000u);   /* dark box */
    ad(G_PRIM, 6ull);
    for (unsigned c = 0; c < 6u; c++)
        for (unsigned y = 0; y < 7u; y++)
            for (unsigned x = 0; x < 5u; x++)
                if (g_osd_font[word[c]][y] & (0x10u >> x)) {
                    const float gx = x0 + (float)(c * 6u + x) * px, gy = y0 + (float)y * px;
                    osd_rect(gx, gy, gx + px, gy + px, 0xFFFFFFFFu);
                }
    const unsigned dots = (unsigned)(g_osd_tick / 15) % 4u;         /* 0..3 dots, 4 per second */
    for (unsigned d = 0; d < dots; d++) {
        const float gx = x0 + (37.0f + (float)d * 2.5f) * px, gy = y0 + 6.0f * px;
        osd_rect(gx, gy, gx + px, gy + px, 0xFFFFFFFFu);
    }
    gif_flush();
}

/* sceDisplaySetFrameBuf: a frame is done, show this PSP framebuffer. */
void ps2_ge_frame(uint32_t fb, uint32_t sync) {
#ifndef GE_HOST_TEST
    ps2_bga_frame();
#endif
    const uint32_t now = ge_cycles();
    W.cyc += now - g_cyc_last;
    g_cyc_last = now;
    g_frames++;
    g_tick_frame = g_tick;
    if (g_dump) psp_printf("[dump] end frame %u\n", g_frames);
    g_dump = g_dump_req; g_dump_req = 0; g_dump_i = 0;
    if (g_dump) psp_printf("[dump] begin frame %u\n", g_frames + 1u);
    if (S.prims) {
        g_frames_drawn++;
        W.frames++;
        W.texmiss += S.texmiss;
        W.up_bytes += S.up_bytes;
        W.evict += S.evict;
        W.up_cyc += S.up_cyc;
        if (S.wset > W.wset_max) W.wset_max = S.wset;
        if (g_frames_drawn <= 3u || g_frames_drawn % 300u == 0u) report_frame();
    }
    memset(&S, 0, sizeof S);
    if (g_osd_frames > 0) { g_osd_frames--; g_osd_tick++; osd_draw(fb & 0xFFFFFFu); }
    const uint64_t dispfb = (uint64_t)((fb & 0xFFFFFFu) ? FB_PAGES : 0u) | ((uint64_t)(SCR_W / 64) << 9) | ((uint64_t)FB_PSM << 15);
    if (sync == 0u) {                        /* IMMEDIATE */
        g_dispfb_pending = 0;
        GS_DISPFB1 = dispfb;
        static uint32_t n;
        if (n++ < 4) psp_printf("[disp] immediate flip fb=0x%06X\n", fb & 0xFFFFFFu);
    } else {
        g_dispfb_next = dispfb;
        g_dispfb_pending = 1;
    }
}

uint32_t ps2_ge_frames_drawn(void) { return g_frames_drawn; }
