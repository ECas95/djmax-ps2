/* ge_tex.c -- PSP GE texture formats -> RGBA8888 + PICA200 tiling.
 *
 * The pixel decoders are ported from port/ps2/ps2_ge.c (same GE formats), with
 * the PSP row swizzle and the CLUT handling kept identical. New here: the
 * PICA200 8x8 Morton tiling (algorithm from P3P3DS gpu_renderer.cpp
 * tiled_index(), MIT) and a self-contained PNG/raw dump for evidence.
 */
#include "ge_tex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- helpers -------------------------------------------------------------- */
static const uint8_t g_bpp[8] = {16, 16, 16, 32, 4, 8, 16, 32};

int ge_tex_bpp(int psm) {
    if (psm >= 0 && psm <= 7) return g_bpp[psm];
    if (psm == GE_PSM_DXT1) return 4;
    if (psm == GE_PSM_DXT3 || psm == GE_PSM_DXT5) return 8;
    return 0;
}

static inline uint32_t pack(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    return (r & 255u) | ((g & 255u) << 8) | ((b & 255u) << 16) | ((a & 255u) << 24);
}

/* One source texel of a linear (non-indexed) format. */
static uint32_t decode_texel(int psm, uint32_t v) {
    switch (psm) {
    case GE_PSM_5650:
        return pack((v & 31u) * 255u / 31u, ((v >> 5) & 63u) * 255u / 63u,
                    ((v >> 11) & 31u) * 255u / 31u, 255u);
    case GE_PSM_5551:
        return pack((v & 31u) * 255u / 31u, ((v >> 5) & 31u) * 255u / 31u,
                    ((v >> 10) & 31u) * 255u / 31u, (v >> 15) ? 255u : 0u);
    case GE_PSM_4444:
        return pack((v & 15u) * 17u, ((v >> 4) & 15u) * 17u,
                    ((v >> 8) & 15u) * 17u, ((v >> 12) & 15u) * 17u);
    case GE_PSM_8888:
        return pack(v & 255u, (v >> 8) & 255u, (v >> 16) & 255u, (v >> 24) & 255u);
    default:
        return 0xFF00FF00u;   /* magenta: unhandled */
    }
}

/* PSP row layout: inside each 8-row group, 16-byte blocks are stored column by
 * column (port/ps2/ps2_ge.c row_gather). */
static void gather_row(uint8_t *d, const uint8_t *src, int swizzled,
                       uint32_t pitch_bytes, uint32_t y, uint32_t bytes) {
    if (!swizzled) {
        memcpy(d, src + (size_t)pitch_bytes * y, bytes);
        return;
    }
    const uint32_t blocks = (pitch_bytes + 15u) / 16u;
    const uint8_t *p = src + (size_t)(y / 8u) * blocks * 128u + (y % 8u) * 16u;
    for (uint32_t i = 0; i < bytes; i += 16u, p += 128u) memcpy(d + i, p, 16u);
}

/* ---- DXT ------------------------------------------------------------------ */
/* PSP DXT block layout differs from PC DDS: colour indices first, then the
 * endpoints, then the alpha block (indices before endpoints for DXT5). */
static void dxt_block(const uint8_t *b, int fmt, uint32_t *out, uint32_t pitch) {
    const uint32_t c0 = b[4] | ((uint32_t)b[5] << 8), c1 = b[6] | ((uint32_t)b[7] << 8);
    uint32_t col[4];
    const uint32_t r0 = (c0 >> 11) & 31u, g0 = (c0 >> 5) & 63u, b0 = c0 & 31u;
    const uint32_t r1 = (c1 >> 11) & 31u, g1 = (c1 >> 5) & 63u, b1 = c1 & 31u;
    col[0] = pack(r0 * 255u / 31u, g0 * 255u / 63u, b0 * 255u / 31u, 255u);
    col[1] = pack(r1 * 255u / 31u, g1 * 255u / 63u, b1 * 255u / 31u, 255u);
    if (c0 > c1) {
        col[2] = pack((2u * r0 + r1) * 255u / 93u, (2u * g0 + g1) * 255u / 189u,
                      (2u * b0 + b1) * 255u / 93u, 255u);
        col[3] = pack((r0 + 2u * r1) * 255u / 93u, (g0 + 2u * g1) * 255u / 189u,
                      (b0 + 2u * b1) * 255u / 93u, 255u);
    } else {
        col[2] = pack((r0 + r1) * 255u / 62u, (g0 + g1) * 255u / 126u,
                      (b0 + b1) * 255u / 62u, 255u);
        col[3] = 0u;
    }
    uint32_t bits = b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    uint32_t *row = out;
    for (uint32_t y = 0; y < 4u; y++, row += pitch)
        for (uint32_t x = 0; x < 4u; x++, bits >>= 2) row[x] = col[bits & 3u];

    if (fmt == GE_PSM_DXT3) {
        row = out;
        for (uint32_t y = 0; y < 4u; y++, row += pitch) {
            uint32_t v = b[8u + y * 2u] | ((uint32_t)b[9u + y * 2u] << 8);
            for (uint32_t x = 0; x < 4u; x++, v >>= 4)
                row[x] = (row[x] & 0xFFFFFFu) | (((v & 15u) * 17u) << 24);
        }
    } else if (fmt == GE_PSM_DXT5) {
        const uint32_t a0 = b[14], a1 = b[15];
        uint32_t at[8];
        at[0] = a0; at[1] = a1;
        if (a0 > a1) {
            for (uint32_t i = 2; i < 8u; i++) at[i] = ((8u - i) * a0 + (i - 1u) * a1) / 7u;
        } else {
            for (uint32_t i = 2; i < 6u; i++) at[i] = ((6u - i) * a0 + (i - 1u) * a1) / 5u;
            at[6] = 0u; at[7] = 255u;
        }
        row = out;
        for (uint32_t y = 0; y < 4u; y++, row += pitch) {
            uint32_t v = y < 2u ? (b[8] | ((uint32_t)b[9] << 8) | ((uint32_t)b[10] << 16))
                                : (b[11] | ((uint32_t)b[12] << 8) | ((uint32_t)b[13] << 16));
            if (y & 1u) v >>= 12;
            for (uint32_t x = 0; x < 4u; x++, v >>= 3)
                row[x] = (row[x] & 0xFFFFFFu) | (at[v & 7u] << 24);
        }
    }
}

/* ---- decode --------------------------------------------------------------- */
int ge_tex_decode(const uint8_t *src, int w, int h, int psm, uint32_t stride,
                  const uint8_t *clut, int clut_psm, int clut_shift, int clut_mask,
                  int swizzled, ge_tex_image *out) {
    if (!src || !out || w <= 0 || h <= 0 || w > 1024 || h > 1024) return -1;
    if (ge_tex_bpp(psm) == 0) return -2;
    if (!stride) stride = (uint32_t)w;

    uint32_t *rgba = (uint32_t *)malloc((size_t)w * (size_t)h * 4u);
    if (!rgba) return -1;
    memset(rgba, 0, (size_t)w * (size_t)h * 4u);

    if (psm >= GE_PSM_DXT1) {
        const uint32_t bsize = (psm == GE_PSM_DXT1) ? 8u : 16u;
        const uint32_t bw = (uint32_t)(w + 3) / 4u, bh = (uint32_t)(h + 3) / 4u;
        for (uint32_t by = 0; by < bh; by++) {
            for (uint32_t bx = 0; bx < bw; bx++) {
                const uint8_t *blk = src + ((size_t)by * bw + bx) * bsize;
                uint32_t tmp[16];
                dxt_block(blk, psm, tmp, 4u);
                for (uint32_t y = 0; y < 4u; y++) {
                    const uint32_t dy = by * 4u + y;
                    if (dy >= (uint32_t)h) break;
                    for (uint32_t x = 0; x < 4u; x++) {
                        const uint32_t dx = bx * 4u + x;
                        if (dx >= (uint32_t)w) break;
                        rgba[(size_t)dy * w + dx] = tmp[y * 4u + x];
                    }
                }
            }
        }
        out->rgba = rgba; out->w = w; out->h = h;
        return 0;
    }

    const uint32_t bpp = (uint32_t)ge_tex_bpp(psm);
    const uint32_t pitch_bytes = stride * bpp / 8u;
    /* w*bpp is in BITS: convert to bytes before rounding for swizzled blocks.
     * The previous expression allocated/read 8x too many bytes for RGBA8888. */
    const uint32_t row_bytes = ((uint32_t)w * bpp + 7u) / 8u;
    const uint32_t aligned_row_bytes = (row_bytes + 15u) & ~15u;
    uint8_t *row = (uint8_t *)malloc(aligned_row_bytes);
    if (!row) { free(rgba); return -1; }

    /* CLUT palette for the indexed formats. */
    uint32_t pal[256];
    int n_pal = 0;
    if (psm == GE_PSM_T4) n_pal = 16;
    else if (psm == GE_PSM_T8 || psm == GE_PSM_T16) n_pal = 256;
    if (n_pal) {
        if (!clut) { free(row); free(rgba); return -3; }
        for (int i = 0; i < n_pal; i++) {
            const uint32_t v = (clut_psm == GE_PSM_8888)
                ? ((const uint32_t *)clut)[i]
                : ((const uint16_t *)clut)[i];
            pal[i] = decode_texel(clut_psm, v);
        }
    }
    if (psm == GE_PSM_T32) { free(row); free(rgba); return -2; }  /* needs 1024-entry CLUT */

    const uint32_t sh = (uint32_t)(clut_shift < 0 ? 0 : clut_shift);
    const uint32_t mask = (uint32_t)(clut_mask <= 0 ? 255 : clut_mask);

    for (int y = 0; y < h; y++) {
        gather_row(row, src, swizzled, pitch_bytes, (uint32_t)y,
                   swizzled ? aligned_row_bytes : row_bytes);
        for (int x = 0; x < w; x++) {
            uint32_t v;
            switch (psm) {
            case GE_PSM_5650: case GE_PSM_5551: case GE_PSM_4444:
                v = ((const uint16_t *)row)[x];
                break;
            case GE_PSM_8888:
                v = ((const uint32_t *)row)[x];
                break;
            case GE_PSM_T4: {
                const uint32_t byte = row[x >> 1];
                const uint32_t idx = (byte >> ((x & 1) ? 4 : 0)) & 15u;
                v = pal[((idx >> sh) & mask) & (n_pal - 1)];
                break;
            }
            case GE_PSM_T8: {
                const uint32_t idx = row[x];
                v = pal[((idx >> sh) & mask) & (n_pal - 1)];
                break;
            }
            default: v = 0xFF00FF00u; break;
            }
            rgba[(size_t)y * w + x] = (psm == GE_PSM_T4 || psm == GE_PSM_T8 ||
                                       psm == GE_PSM_T16) ? v : decode_texel(psm, v);

        }
    }
    free(row);
    out->rgba = rgba; out->w = w; out->h = h;
    return 0;
}

void ge_tex_free(ge_tex_image *img) {
    if (!img) return;
    free(img->rgba);
    img->rgba = NULL;
    img->w = img->h = 0;
}

/* ---- PICA200 tiling ------------------------------------------------------- */
static inline uint32_t morton3(uint32_t x, uint32_t y) {
    return (x & 1u) | ((y & 1u) << 1) | ((x & 2u) << 1) | ((y & 2u) << 2)
         | ((x & 4u) << 2) | ((y & 4u) << 3);
}

uint32_t ge_tex_tiled_index(uint32_t x, uint32_t y, uint32_t w) {
    const uint32_t bw = (w + 7u) / 8u;
    return ((y / 8u) * bw + (x / 8u)) * 64u + morton3(x & 7u, y & 7u);
}

int ge_tex_to_pica_rgba8(const ge_tex_image *img, uint8_t *dst) {
    if (!img || !img->rgba || !dst) return -1;
    for (int y = 0; y < img->h; y++) {
        for (int x = 0; x < img->w; x++) {
            const uint32_t v = img->rgba[(size_t)y * img->w + x];
            const uint32_t o = ge_tex_tiled_index((uint32_t)x, (uint32_t)y, (uint32_t)img->w) * 4u;
            /* GPU_RGBA8 in PICA200 is stored in A,B,G,R byte order. */
            dst[o + 0] = (uint8_t)((v >> 24) & 255u);
            dst[o + 1] = (uint8_t)((v >> 16) & 255u);
            dst[o + 2] = (uint8_t)((v >> 8) & 255u);
            dst[o + 3] = (uint8_t)(v & 255u);
        }
    }
    return 0;
}

/* ---- dumps ---------------------------------------------------------------- */
static uint32_t crc_table[256];
static int crc_ready;
static void crc_init(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
    crc_ready = 1;
}
static uint32_t crc32_buf(const uint8_t *p, size_t n) {
    if (!crc_ready) crc_init();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 255u] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
static uint32_t adler32_buf(const uint8_t *p, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) { a = (a + p[i]) % 65521u; b = (b + a) % 65521u; }
    return (b << 16) | a;
}
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static uint32_t chunk(uint8_t *o, uint32_t at, const char *tag, const uint8_t *d, uint32_t n) {
    put32(o + at, n); memcpy(o + at + 4, tag, 4);
    if (n) memcpy(o + at + 8, d, n);
    put32(o + at + 8 + n, crc32_buf(o + at + 4, 4u + n));
    return 12u + n;
}

int ge_tex_dump_png(const char *path, const ge_tex_image *img) {
    if (!img || !img->rgba || img->w <= 0 || img->h <= 0) return -1;
    const size_t row = (size_t)img->w * 4u;
    const size_t raw_len = (row + 1u) * (size_t)img->h;
    uint8_t *raw = (uint8_t *)malloc(raw_len);
    if (!raw) return -1;
    for (int y = 0; y < img->h; y++) {
        raw[(row + 1u) * (size_t)y] = 0;
        memcpy(raw + (row + 1u) * (size_t)y + 1u,
               (const uint8_t *)img->rgba + (size_t)y * row, row);
    }
    const size_t blocks = (raw_len + 65534u) / 65535u;
    const size_t zlen = 2u + raw_len + blocks * 5u + 4u;
    uint8_t *z = (uint8_t *)malloc(zlen);
    if (!z) { free(raw); return -1; }
    size_t zn = 0;
    z[zn++] = 0x78; z[zn++] = 0x01;
    size_t off = 0;
    while (off < raw_len) {
        const size_t n = (raw_len - off > 65535u) ? 65535u : (raw_len - off);
        z[zn++] = (uint8_t)((off + n >= raw_len) ? 1 : 0);
        z[zn++] = (uint8_t)(n & 255u); z[zn++] = (uint8_t)(n >> 8);
        z[zn++] = (uint8_t)(~n & 255u); z[zn++] = (uint8_t)((~n >> 8) & 255u);
        memcpy(z + zn, raw + off, n); zn += n; off += n;
    }
    put32(z + zn, adler32_buf(raw, raw_len)); zn += 4;

    uint8_t ihdr[13];
    put32(ihdr, (uint32_t)img->w); put32(ihdr + 4, (uint32_t)img->h);
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    const size_t total = 8u + 25u + (12u + zn) + 12u;
    uint8_t *o = (uint8_t *)malloc(total);
    if (!o) { free(raw); free(z); return -1; }
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    memcpy(o, sig, 8);
    uint32_t at = 8;
    at += chunk(o, at, "IHDR", ihdr, 13);
    at += chunk(o, at, "IDAT", z, (uint32_t)zn);
    at += chunk(o, at, "IEND", NULL, 0);
    FILE *f = fopen(path, "wb");
    int rc = -1;
    if (f) { if (fwrite(o, 1, at, f) == at) rc = 0; fclose(f); }
    free(raw); free(z); free(o);
    return rc;
}

int ge_tex_dump_raw(const char *path, const void *data, uint32_t bytes) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    const int rc = (fwrite(data, 1, bytes, f) == bytes) ? 0 : -1;
    fclose(f);
    return rc;
}
