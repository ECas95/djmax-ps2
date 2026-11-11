/* ge_tex.h -- PSP GE texture decoding to linear RGBA8888 + PICA200 tiling.
 *
 * Formats (PSP GU_PSM_* values, same numbering the GE writes in TEXFORMAT):
 *   0 5650 (RGB565, alpha 1)      1 5551        2 4444        3 8888
 *   4 T4  + CLUT (16 colours)     5 T8 + CLUT (256 colours)
 *   6 T16 + CLUT (unsupported)    7 T32 + CLUT (unsupported)
 *   8 DXT1                        9 DXT3       10 DXT5
 *
 * Source bytes are read from a caller-supplied buffer (guest memory read by the
 * caller). When `swizzled` is set the PSP row layout is undone first: within
 * each group of 8 rows the 16-byte blocks are stored column by column.
 *
 * Decoded RGBA8888 is packed as r | g<<8 | b<<16 | a<<24.
 * The PICA upload helper converts to GPU_RGBA8 byte order A,B,G,R.
 */
#ifndef DJMX_3DS_GE_TEX_H
#define DJMX_3DS_GE_TEX_H

#include <stdint.h>

enum {
    GE_PSM_5650 = 0, GE_PSM_5551 = 1, GE_PSM_4444 = 2, GE_PSM_8888 = 3,
    GE_PSM_T4 = 4, GE_PSM_T8 = 5, GE_PSM_T16 = 6, GE_PSM_T32 = 7,
    GE_PSM_DXT1 = 8, GE_PSM_DXT3 = 9, GE_PSM_DXT5 = 10
};

typedef struct {
    uint32_t *rgba;   /* w*h texels, r | g<<8 | b<<16 | a<<24 */
    int w, h;
} ge_tex_image;

/* Bytes per texel of the source for the linear formats (0 for DXT/unsupported). */
int ge_tex_bpp(int psm);

/* Decodes one texture. `stride` is in texels per row (TEXBUFWIDTH0); pass 0 to
 * use w. CLUT parameters come from TEXCLUT (cfmt/shift/mask/start) and CLUTADDR.
 * `clut_start` is the bank base in 16-entry units (reg 0xC5 bits 16-20); the
 * caller must supply a CLUT buffer covering start*16 + 16/256 entries.
 * Returns 0 on success, <0 on bad arguments/unsupported format. */
int ge_tex_decode(const uint8_t *src, int w, int h, int psm, uint32_t stride,
                  const uint8_t *clut, int clut_psm, int clut_shift, int clut_mask,
                  int clut_start, int swizzled, ge_tex_image *out);

void ge_tex_free(ge_tex_image *img);

/* Copies the image into a PICA200 RGBA8 texture layout: 8x8 Morton tiles,
 * tiles in row-major order. `dst` must hold w*h*4 bytes. */
int ge_tex_to_pica_rgba8(const ge_tex_image *img, uint8_t *dst);

/* Offset in bytes of texel (x,y) in a PICA tiled surface of width w.
 * (Same algorithm as P3P3DS gpu_renderer.cpp tiled_index(), MIT.) */
uint32_t ge_tex_tiled_index(uint32_t x, uint32_t y, uint32_t w);

/* Dump helpers (sdmc paths). PNG is colour type 6 (RGBA8). */
int ge_tex_dump_png(const char *path, const ge_tex_image *img);
int ge_tex_dump_raw(const char *path, const void *data, uint32_t bytes);

#endif
