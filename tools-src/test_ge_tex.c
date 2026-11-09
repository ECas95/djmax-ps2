/* test_ge_tex.c -- host test of the texture path in port/ps2/ps2_ge.c.
 *
 * Includes the real file with the hardware block replaced: a DMA kick here parses the
 * GIF packet and records transfers, so what is checked is the byte stream the GS would get.
 *
 *   test_ge_tex gim <file.gim> <expected.rgba> <w> <h> <fmt> <pix_off> <pal_off> <swz>
 *   test_ge_tex cache
 *
 * Driven by test_ge_tex.py, which gets expected.rgba from gim2png.py. Exit 0 is a pass.
 */
#define GE_HOST_TEST
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* What the file takes from the hardware block. */
static volatile uint64_t g_fake_reg;
#define R64(a) g_fake_reg
#define R32(a) g_fake_reg
#define SetGsCrt(a, b, c) ((void)0)
#define ExitHandler() ((void)0)
#define AddIntcHandler(a, b, c) ((void)0)
#define EnableIntc(a) ((void)0)
static void dma_kick(const void *p, uint32_t qwc);
static uint32_t ge_cycles(void) { return 0; }

#include GE_SRC

/* ---- what the test provides of the runtime --------------------------------- */
#define GUEST_BASE 0x08800000u
#define GUEST_SIZE (8u << 20)
static uint8_t g_mem[GUEST_SIZE];
uint64_t psp_mem_bad_access;
void *psp_mem_ptr(uint32_t addr, uint32_t size) {
    return addr >= GUEST_BASE && addr - GUEST_BASE + size <= GUEST_SIZE ? g_mem + (addr - GUEST_BASE) : NULL;
}
uint8_t psp_read8(uint32_t a) { uint8_t *p = psp_mem_ptr(a, 1); return p ? *p : 0; }
uint16_t psp_read16(uint32_t a) { uint16_t v = 0; void *p = psp_mem_ptr(a, 2); if (p) memcpy(&v, p, 2); return v; }
uint32_t psp_read32(uint32_t a) { uint32_t v = 0; void *p = psp_mem_ptr(a, 4); if (p) memcpy(&v, p, 4); return v; }
float psp_read_f32(uint32_t a) { uint32_t v = psp_read32(a); float f; memcpy(&f, &v, 4); return f; }
void psp_printf(const char *fmt, ...) { (void)fmt; }
uint64_t psp_dispatch_misses(void) { return 0; }
uint64_t psp_hle_total_calls(void) { return 0; }

/* ---- a GS that only listens ------------------------------------------------- */
typedef struct { uint32_t dbp, dbw, psm, w, h, off, len, kicks, eop; } xfer;
static xfer g_x[8];
static int g_nx;
static uint8_t g_img[4u << 20];
static uint32_t g_img_n;
static uint64_t g_bitblt, g_trxreg, g_tex0, g_flushes;

static void dma_kick(const void *p, uint32_t qwc) {
    const uint64_t *q = p;
    const uint32_t nloop = (uint32_t)(q[0] & 0x7FFFu), flg = (uint32_t)((q[0] >> 58) & 3u);
    assert(nloop + 1u == qwc);
    if (flg == 2u) {
        xfer *x = &g_x[g_nx - 1];
        assert(g_nx > 0 && !x->eop);
        memcpy(g_img + g_img_n, &q[2], nloop * 16u);
        g_img_n += nloop * 16u;
        x->len += nloop * 16u;
        x->kicks++;
        x->eop = (uint32_t)((q[0] >> 15) & 1u);
        return;
    }
    for (uint32_t i = 1; i <= nloop; i++) {
        const uint64_t v = q[i * 2u];
        switch (q[i * 2u + 1u]) {
        case G_BITBLTBUF: g_bitblt = v; break;
        case G_TRXREG: g_trxreg = v; break;
        case G_TEX0: g_tex0 = v; break;
        case G_TEXFLUSH: g_flushes++; break;
        case G_TRXDIR: {
            xfer *x = &g_x[g_nx++];
            memset(x, 0, sizeof *x);
            x->dbp = (uint32_t)(g_bitblt >> 32) & 0x3FFFu;
            x->dbw = (uint32_t)(g_bitblt >> 48) & 63u;
            x->psm = (uint32_t)(g_bitblt >> 56) & 63u;
            x->w = (uint32_t)g_trxreg;
            x->h = (uint32_t)(g_trxreg >> 32);
            x->off = g_img_n;
            break;
        }
        default: break;
        }
    }
}

static void xfers_reset(void) { g_nx = 0; g_img_n = 0; }

/* Every transfer must carry exactly its area, with EOP on its last chunk. */
static void xfers_check(void) {
    for (int i = 0; i < g_nx; i++) {
        const xfer *x = &g_x[i];
        const uint32_t bits = x->psm == PSM_CT32 ? 32u : x->psm == PSM_CT16 ? 16u : x->psm == PSM_T8 ? 8u : 4u;
        assert(x->w * x->h * bits == x->len * 8u);
        assert(x->eop);
    }
}

static void set_tex(uint32_t addr, uint32_t stride, uint32_t fmt, uint32_t swz, uint32_t lw, uint32_t lh) {
    g_r[0xA0] = addr & 0xFFFFFFu;
    g_r[0xA8] = ((addr >> 8) & 0xFF0000u) | stride;
    g_r[0xC3] = fmt;
    g_r[0xC2] = swz;
    g_r[0xB8] = lw | (lh << 8);
}

/* ---- a real GIM against what gim2png.py makes of it ------------------------- */
static int test_gim(char **a) {
    const uint32_t w = (uint32_t)atoi(a[2]), h = (uint32_t)atoi(a[3]), fmt = (uint32_t)atoi(a[4]);
    const uint32_t pix = (uint32_t)atoi(a[5]), pal = (uint32_t)atoi(a[6]), swz = (uint32_t)atoi(a[7]);
    FILE *f = fopen(a[0], "rb");
    assert(f);
    const size_t n = fread(g_mem, 1, GUEST_SIZE, f);
    fclose(f);
    (void)n;
    uint8_t *want = malloc(w * h * 4u);
    f = fopen(a[1], "rb");
    assert(f && fread(want, 1, w * h * 4u, f) == w * h * 4u);
    fclose(f);

    uint32_t lw = 0, lh = 0;
    while ((1u << lw) < w) lw++;
    while ((1u << lh) < h) lh++;
    set_tex(GUEST_BASE + pix, w, fmt, swz, lw, lh);
    const uint32_t ca = GUEST_BASE + pal;
    g_r[0xB0] = ca & 0xFFFFFFu;
    g_r[0xB1] = (ca >> 8) & 0xFF0000u;
    g_r[0xC5] = 3u | (0xFFu << 8);     /* CLUT 8888, shift 0, mask FF, start 0 */
    g_r[0xC9] = 1u << 8;

    gs_set_draw_state(1, 0);
    gif_flush();
    xfers_check();

    const int clut = fmt == 4u || fmt == 5u;
    assert(g_nx == (clut ? 2 : 1));
    const xfer *img = &g_x[g_nx - 1];
    const uint8_t *data = g_img + img->off;

    /* TEX0 must agree with the transfer. */
    assert((g_tex0 & 0x3FFFu) == img->dbp);
    assert(((g_tex0 >> 14) & 63u) == img->dbw);
    assert(((g_tex0 >> 20) & 63u) == img->psm);
    assert(((g_tex0 >> 26) & 15u) == lw && ((g_tex0 >> 30) & 15u) == lh);
    assert(g_flushes == 1u);
    if (clut) {
        assert(((g_tex0 >> 37) & 0x3FFFu) == g_x[0].dbp);
        assert((g_tex0 >> 61) == 1u && ((g_tex0 >> 51) & 15u) == 0u && ((g_tex0 >> 55) & 1u) == 0u);
        assert(g_x[0].psm == PSM_CT32 && g_x[0].w == 16u && g_x[0].h == 16u && !(g_x[0].dbp & 3u));
    }

    uint32_t bad = 0, first_x = 0, first_y = 0;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *e = want + (y * w + x) * 4u;
            const uint32_t exp = e[0] | ((uint32_t)e[1] << 8) | ((uint32_t)e[2] << 16)
                               | (((e[3] + 1u) >> 1) << 24);
            const uint32_t row = img->w * (img->psm == PSM_CT32 ? 32u : img->psm == PSM_CT16 ? 16u : 8u) / 8u;
            uint32_t got;
            if (img->psm == PSM_CT32) {
                memcpy(&got, data + y * row + x * 4u, 4);
            } else if (img->psm == PSM_CT16) {
                uint16_t v;
                memcpy(&v, data + y * row + x * 2u, 2);
                got = ((v & 31u) * 255u / 31u) | (((v >> 5) & 31u) * 255u / 31u) << 8
                    | (((v >> 10) & 31u) * 255u / 31u) << 16 | (v >> 15 ? 0x80000000u : 0u);
            } else {
                /* T8 through the uploaded CLUT, undoing the CSM1 order on our own. */
                const uint32_t i = data[y * row + x];
                const uint32_t b34 = i & 0x18u;
                const uint32_t pos = b34 == 8u ? i + 8u : b34 == 16u ? i - 8u : i;
                memcpy(&got, g_img + g_x[0].off + pos * 4u, 4);
            }
            if (got != exp && !bad++) { first_x = x; first_y = y; }
        }
    }
    printf("fmt=%u %ux%u swz=%u psm=0x%02X tbw=%u kicks=%u bad=%u", fmt, w, h, swz, img->psm, img->dbw, img->kicks, bad);
    if (bad) printf(" first at %u,%u", first_x, first_y);
    printf("\n");

    /* Bound again, unchanged: no second upload. */
    const uint32_t up = S.texmiss;
    gs_set_draw_state(1, 0);
    assert(S.texmiss == up);
    return bad != 0;
}

/* ---- cache behaviour -------------------------------------------------------- */
/* The page map must agree with the entries, and hold each texture in one run. */
static void cache_invariant(void) {
    uint32_t seen = 0;
    for (uint32_t i = 0; i < TC_MAX; i++) {
        if (!g_tc[i].npg) continue;
        assert(g_tc[i].pg + g_tc[i].npg <= POOL_PAGES);
        for (uint32_t p = 0; p < g_tc[i].npg; p++) assert(g_pg[g_tc[i].pg + p] == i + 1u);
        seen += g_tc[i].npg;
    }
    uint32_t marked = 0;
    for (uint32_t p = 0; p < POOL_PAGES; p++) marked += g_pg[p] != 0;
    assert(marked == seen);
}

/* Texture number n: 256x256 CT32, 32 pages, so FIT of them fit. Returns 1 on upload. */
#define FIT (POOL_PAGES / 32u)
static int bind_big(uint32_t n) {
    const uint32_t before = S.texmiss;
    const uint32_t addr = GUEST_BASE + n * 0x40000u;
    memcpy(g_mem + n * 0x40000u, &n, 4);
    set_tex(addr, 256u, 3u, 0u, 8u, 8u);
    assert(tex_bind());
    xfers_reset();
    cache_invariant();
    return S.texmiss != before;
}

static void new_frame(void) {
    gif_flush();
    ps2_ge_frame(0, 0);
}

/* Bytes of GS memory really needed against pages asked for, by the GS's own page shapes. */
static void geometry(uint32_t fmt, uint32_t lw, uint32_t lh, uint32_t want_pg, uint32_t want_tbw, uint32_t want_psm) {
    memset(g_tc, 0, sizeof g_tc);
    memset(g_pg, 0, sizeof g_pg);
    set_tex(GUEST_BASE + 0x100000u, 1u << lw, fmt, 0u, lw, lh);
    const tc_ent *e = tex_bind();
    assert(e);
    gif_flush();
    assert(e->npg == want_pg);
    assert(((e->tex0 >> 14) & 63u) == want_tbw);
    assert(((e->tex0 >> 20) & 63u) == want_psm);
    assert((e->tex0 & 0x3FFFu) == (POOL_BASE + e->pg) * 32u);
    assert(g_x[g_nx - 1].dbw == want_tbw);
    xfers_check();
    xfers_reset();
}

static int test_cache(void) {
    g_r[0xC5] = 3u | (0xFFu << 8);

#if !GE_FB16
    assert(POOL_PAGES == 224u);
#else
    assert(POOL_PAGES == 364u);
#endif
    assert(CLUT_BASE / 32u + CLUT_PAGES == POOL_BASE);
    /* T8 512x256 is 128 KB: 16 pages. */
    geometry(5u, 9u, 8u, 16u, 8u, PSM_T8);
    geometry(5u, 7u, 7u, 2u, 2u, PSM_T8);
    /* T4 128x128 is 8 KB: 1 page, TBW still 2. */
    geometry(4u, 7u, 7u, 1u, 2u, PSM_T4);
    geometry(1u, 6u, 6u, 1u, 1u, PSM_CT16);
    geometry(0u, 8u, 8u, 16u, 4u, PSM_CT16);
    /* CT32 512x256 is 512 KB: 64 pages; DXT lands there too. */
    geometry(3u, 9u, 8u, 64u, 8u, PSM_CT32);
    geometry(10u, 9u, 8u, 64u, 8u, PSM_CT32);
    /* Tiny: rows rounded up to a qword, still one page. */
    geometry(2u, 0u, 0u, 1u, 1u, PSM_CT32);
    geometry(4u, 1u, 1u, 1u, 2u, PSM_T4);
    geometry(10u, 1u, 0u, 1u, 1u, PSM_CT32);
    memset(g_tc, 0, sizeof g_tc);
    memset(g_pg, 0, sizeof g_pg);

    /* 1. A working set that fits: uploads once, then never. */
    for (uint32_t i = 0; i < FIT; i++) assert(bind_big(i) == 1);
    new_frame();
    for (int f = 0; f < 3; f++) {
        for (uint32_t i = 0; i < FIT; i++) assert(bind_big(i) == 0);
        new_frame();
    }

    /* 2. A new one evicts the oldest not used this frame, not those just used. */
    assert(bind_big(0) == 0);
    assert(bind_big(1) == 0);
    assert(bind_big(FIT) == 1);        /* evicts 2 */
    assert(bind_big(0) == 0);
    assert(bind_big(1) == 0);
    for (uint32_t i = 3; i < FIT; i++) assert(bind_big(i) == 0);
    assert(bind_big(2) == 1);          /* it really went; every other was used this frame */
    new_frame();

    /* 3. No alternative: all used this frame, so the most recent goes, not the oldest. */
    memset(g_tc, 0, sizeof g_tc);
    memset(g_pg, 0, sizeof g_pg);
    for (uint32_t i = 0; i < FIT; i++) assert(bind_big(i) == 1);
    assert(bind_big(FIT) == 1);        /* evicts FIT-1 */
    assert(bind_big(0) == 0);
    assert(bind_big(FIT - 2u) == 0);
    new_frame();

    /* 4. A cycle one bigger than the cache must not miss on every bind. */
    memset(g_tc, 0, sizeof g_tc);
    memset(g_pg, 0, sizeof g_pg);
    uint32_t misses = 0;
    for (int f = 0; f < 8; f++) {
        memset(&S, 0, sizeof S);
        for (uint32_t i = 0; i < FIT + 1u; i++) bind_big(i);
        misses = S.texmiss;
        new_frame();
    }
    printf("cycle of %u in room for %u: %u misses per frame (LRU alone would be %u)\n", FIT + 1u, FIT, misses, FIT + 1u);
    assert(misses <= 3u);

    /* 5. Mixed sizes and formats, far more textures than entries: the map stays sound. */
    memset(g_tc, 0, sizeof g_tc);
    memset(g_pg, 0, sizeof g_pg);
    uint32_t r = 12345u;
    for (int i = 0; i < 20000; i++) {
        r = r * 1103515245u + 12345u;
        const uint32_t n = (r >> 16) % 200u, lw = 3u + (n * 7u % 7u), lh = 3u + (n * 11u % 7u);
        const uint32_t fmt = "\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0A"[n % 11u];
        set_tex(GUEST_BASE + n * 0x8000u, 1u << lw, fmt, n & 1u, lw, lh);
        assert(tex_bind());
        gif_flush();
        xfers_check();
        xfers_reset();
        cache_invariant();
        if (i % 37 == 0) new_frame();
    }
    printf("stress: ok, %u pool pages, %d entries\n", POOL_PAGES, TC_MAX);
    return 0;
}

int main(int argc, char **argv) {
    ps2_ge_init();
    gif_flush();
    xfers_reset();
    if (argc == 2 && !strcmp(argv[1], "cache")) return test_cache();
    if (argc == 10 && !strcmp(argv[1], "gim")) return test_gim(argv + 2);
    fprintf(stderr, "usage: see the top of test_ge_tex.c\n");
    return 2;
}
