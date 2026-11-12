/* ge_draw.c -- PSP GE -> PICA200 (citro3d) renderer for the DJMAX Fever 3DS port.
 *
 * Reads GE display lists out of guest memory (psp_read32), decodes vertices by
 * the full VTYPE, expands them to triangles and draws them with citro3d using
 * vertex colours (magenta by default, to validate geometry before texturing).
 * Textures are recorded in the census but not sampled yet.
 *
 * The vertex shader binary is embedded below (compiled from
 * ge_test/source/vshader.v.pica with picasso): position, texcoord0, colour.
 */
#include "ge_draw.h"
#include "ge_tex.h"
#include "runtime_3ds.h"

#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- embedded vertex shader (picasso: vshader.v.pica) --------------------- */
/* DVLB_ParseFile accepts a u32*; it parses in place, so the storage must be
 * writable and word-aligned. An unsigned-char array alone has alignment 1. */
static unsigned char ge_vshader_shbin[] __attribute__((aligned(4))) = {
    0x44, 0x56, 0x4C, 0x42, 0x01, 0x00, 0x00, 0x00, 0x90, 0x00, 0x00, 0x00,
    0x44, 0x56, 0x4C, 0x50, 0x00, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00,
    0x09, 0x00, 0x00, 0x00, 0x4C, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00,
    0x84, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4E, 0x01, 0xF0, 0x07, 0x4E,
    0x02, 0x08, 0x02, 0x08, 0x03, 0x18, 0x02, 0x08, 0x04, 0x28, 0x02, 0x08,
    0x05, 0x38, 0x02, 0x08, 0x06, 0x10, 0x20, 0x4C, 0x06, 0x20, 0x40, 0x4C,
    0x00, 0x00, 0x00, 0x88, 0x6E, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xA1, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x68, 0xC3, 0x06, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x64, 0xC3, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x62, 0xC3, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x61, 0xC3, 0x06, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x6F, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x44, 0x56, 0x4C, 0x45, 0x02, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x54, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x54, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x6C, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x74, 0x00, 0x00, 0x00,
    0x0B, 0x00, 0x00, 0x00, 0x02, 0x00, 0x5F, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0xBF, 0x00, 0x00, 0x00, 0x3E, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x03, 0x00, 0x01, 0x00,
    0x0F, 0x00, 0x00, 0x00, 0x02, 0x00, 0x02, 0x00, 0x0F, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x13, 0x00, 0x70, 0x72, 0x6F, 0x6A,
    0x65, 0x63, 0x74, 0x69, 0x6F, 0x6E, 0x00, 0x00,
};
#define ge_vshader_shbin_size 272u
typedef char ge_shader_word_size_check[(sizeof(ge_vshader_shbin) % sizeof(u32) == 0) ? 1 : -1];
typedef char ge_shader_expected_size_check[(sizeof(ge_vshader_shbin) == ge_vshader_shbin_size) ? 1 : -1];

#define CANVAS_W 400.0f
#define CANVAS_H 240.0f
#define PSP_W    480.0f
#define PSP_H    272.0f
#define LETTERBOX ((CANVAS_H - PSP_H * (CANVAS_W / PSP_W)) * 0.5f)

typedef struct { float x, y, z, u, v, r, g, b, a; } Vtx;

#define VB_VERTS 16384
#define LIST_BUDGET 65536u

/* ---- GE state ------------------------------------------------------------- */
static uint32_t g_r[256];
static uint32_t g_base, g_offset, g_vaddr, g_iaddr;
static float    g_world[12], g_view[12], g_proj[16];
static uint32_t g_wi, g_vi, g_pi;
static uint32_t g_vtype;
static uint32_t g_hist[256];
static uint32_t g_n_vtype, g_n_vtype_nz;

static struct {
    uint32_t lists, cmds, prims, submitted, no_pos, bad_addr, other, textured;
    uint32_t ignored, frames, lists_bad;
} S;

static ge_census_call g_census[GE_CENSUS_MAX];
static int g_census_n;

static int g_magenta = 1;
static void *g_target;            /* C3D_RenderTarget* superior (ojo izq.) */
static void *g_right;             /* C3D_RenderTarget* superior ojo derecho (3D) */
static void *g_bottom;            /* C3D_RenderTarget* inferior (puede ser NULL) */
static uint32_t g_clear = 0x000000FFu;
static int g_frame_open;
static int g_ready;
static int g_route;               /* 0 = superior, 1 = inferior (por batch) */
static int g_split;               /* frames restantes de modo dual (BGA arriba) */
static int g_gear_seen;           /* ya pasó el chasis del gear en este frame */
static int g_layout;              /* SELECT: 0 BGA↑/gear↓ 1 BGA↓/gear↑ 2 todo↓ 3 todo↑ */
typedef struct { float l, r, b, t; } OrthoVP;
static OrthoVP g_vp_top       = { 0.0f, 480.0f, 280.0f, -8.0f };
static OrthoVP g_vp_bot       = { 0.0f, 480.0f, 316.0f, -44.0f };
static OrthoVP g_vp_split     = { 58.67f, 421.33f, 272.0f, 0.0f };
static OrthoVP g_vp_split_top = { 13.33f, 466.67f, 272.0f, 0.0f };
static C3D_Mtx g_mtx_bot;
static C3D_Mtx g_mtx_split;       /* encuadre del gear: inferior */
static C3D_Mtx g_mtx_split_top;   /* encuadre del gear: superior (modo 1) */
static const C3D_Mtx *g_cur_mtx;  /* matriz del batch en curso */
static const OrthoVP *g_cur_vp;
static float g_cur_depth = 3.0f;  /* paralaje (unidades PSP) a slider=1 */
static float g_split_cx = 240.0f;
static C3D_Tex g_logo;            /* logo de la pantalla libre */
static int g_logo_ready;

static Vtx *g_vb;
static int g_vn, g_vbase; /* never reuse submitted memory within one GPU frame */
#define GE_TEXTURE_SLOTS 160
#define GE_TEXTURE_BUDGET (24u * 1024u * 1024u)
typedef struct {
    C3D_Tex tex;
    uint32_t addr, w, h, psm, stride, clut_addr, clut_fmt, clut_cfg, swizzle;
    uint32_t frame_used, frame_loaded;
    uint32_t bytes;
    uint32_t fp;                   /* huella del contenido subido */
    uint32_t ckey;                 /* color test: clave RGB o 0xFFFFFFFF off */
    uint32_t cmask;
    uint32_t last_change;          /* frame del último cambio de contenido (video) */
    int      fp_valid;
    int valid;
} GpuTexSlot;
static GpuTexSlot g_texcache[GE_TEXTURE_SLOTS];
static uint32_t g_tex_bytes, g_tex_hits, g_tex_uploads, g_tex_fail;
static uint32_t g_draw_drop;
/* Read-only preview of the user's own DJMAX PSP_GAME/PIC1.PNG/ICON0.PNG, converted
 * on Windows to RGBA8. Never report this preview as game geometry. */
#define PREVIEW_W 256u
#define PREVIEW_H 128u
static C3D_Tex g_preview_tex;
static int g_preview_ready;
static uint32_t g_guest_vertices_this_frame;
static uint32_t g_preview_frames;
static int g_force_preview;



/* ---- citro3d resources ---------------------------------------------------- */
static DVLB_s *g_dvlb;
static shaderProgram_s g_prog;
static int g_uproj;
static C3D_Mtx g_mtx;

static inline float f24(uint32_t a) {
    union { uint32_t u; float f; } c;
    c.u = a << 8;
    return c.f;
}

/* Size of one vertex in bytes: copied from port/ps2/ps2_ge.c (same GE). */
static uint32_t vertex_size(uint32_t vt) {
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

static inline uint32_t col16(uint32_t fmt, uint32_t v) {
    switch (fmt) {
    case 4: return ((v & 31u) * 255u / 31u) | (((v >> 5) & 63u) * 255u / 63u) << 8
                 | (((v >> 11) & 31u) * 255u / 31u) << 16 | 0xFF000000u;
    case 5: return ((v & 31u) * 255u / 31u) | (((v >> 5) & 31u) * 255u / 31u) << 8
                 | (((v >> 10) & 31u) * 255u / 31u) << 16 | ((v >> 15) ? 0xFF000000u : 0u);
    default: return (v & 15u) * 17u | ((v >> 4) & 15u) * 17u << 8
                  | ((v >> 8) & 15u) * 17u << 16 | ((v >> 12) & 15u) * 17u << 24;
    }
}

static inline void mul43(const float *m, const float *in, float *out) {
    out[0] = in[0] * m[0] + in[1] * m[3] + in[2] * m[6] + m[9];
    out[1] = in[0] * m[1] + in[1] * m[4] + in[2] * m[7] + m[10];
    out[2] = in[0] * m[2] + in[1] * m[5] + in[2] * m[8] + m[11];
}

/* Decode one vertex; returns 1 when it has a usable position. */
static int vtx_read(uint32_t a, uint32_t vt, Vtx *o, int *has_uv) {
    const uint32_t t = vt & 3u, c = (vt >> 2) & 7u, n = (vt >> 5) & 3u, p = (vt >> 7) & 3u;
    const uint32_t w = (vt >> 9) & 3u, wc = ((vt >> 14) & 7u) + 1u;
    const int through = (vt >> 23) & 1u;
    static const uint8_t wsz[4] = {0, 1, 2, 4};
    uint32_t q = a + wsz[w] * (w ? wc : 0u);
    uint32_t col = 0xFFFFFFFFu;
#define AL(al) (q = (q + (al) - 1u) & ~((al) - 1u))
    o->u = o->v = 0.0f;
    *has_uv = 0;
    switch (t) {
    case 1: o->u = (float)psp_read8(q); o->v = (float)psp_read8(q + 1u); q += 2u;
            if (!through) { o->u /= 128.0f; o->v /= 128.0f; } *has_uv = 1; break;
    case 2: AL(2u); o->u = (float)psp_read16(q); o->v = (float)psp_read16(q + 2u); q += 4u;
            if (!through) { o->u /= 32768.0f; o->v /= 32768.0f; } *has_uv = 1; break;
    case 3: AL(4u); o->u = psp_read_f32(q); o->v = psp_read_f32(q + 4u); q += 8u; *has_uv = 1; break;
    default: break;
    }
    if (c >= 4u && c <= 6u) { AL(2u); col = col16(c, psp_read16(q)); q += 2u; }
    else if (c == 7u)       { AL(4u); col = psp_read32(q); q += 4u; }
    else                    { col = (g_r[0x55] & 0xFFFFFFu) | ((g_r[0x58] & 255u) << 24); }
    if (n == 1u) q += 3u; else if (n == 2u) { AL(2u); q += 6u; } else if (n == 3u) { AL(4u); q += 12u; }

    float in[3] = {0, 0, 0};
    switch (p) {
    case 1: in[0] = (float)(int8_t)psp_read8(q); in[1] = (float)(int8_t)psp_read8(q + 1u);
            in[2] = (float)(int8_t)psp_read8(q + 2u); break;
    case 2: AL(2u); in[0] = (float)(int16_t)psp_read16(q); in[1] = (float)(int16_t)psp_read16(q + 2u);
            in[2] = (float)(through ? (uint16_t)psp_read16(q + 4u) : (int16_t)psp_read16(q + 4u)); break;
    case 3: AL(4u); in[0] = psp_read_f32(q); in[1] = psp_read_f32(q + 4u);
            in[2] = psp_read_f32(q + 8u); break;
    default: return 0;
    }
#undef AL

    float sx, sy;
    if (through) {
        sx = in[0]; sy = in[1];
    } else {
        if (p == 1u) { in[0] /= 128.0f; in[1] /= 128.0f; in[2] /= 128.0f; }
        if (p == 2u) { in[0] /= 32768.0f; in[1] /= 32768.0f; in[2] /= 32768.0f; }
        float wv[3], vv[3];
        mul43(g_world, in, wv);
        mul43(g_view, wv, vv);
        const float cw = vv[0] * g_proj[3] + vv[1] * g_proj[7] + vv[2] * g_proj[11] + g_proj[15];
        if (cw <= 0.00001f) return 0;
        const float cx = vv[0] * g_proj[0] + vv[1] * g_proj[4] + vv[2] * g_proj[8] + g_proj[12];
        const float cy = vv[0] * g_proj[1] + vv[1] * g_proj[5] + vv[2] * g_proj[9] + g_proj[13];
        sx = cx / cw * f24(g_r[0x42]) + f24(g_r[0x45]) - (float)(g_r[0x4C] & 0xFFFFu) / 16.0f;
        sy = cy / cw * f24(g_r[0x43]) + f24(g_r[0x46]) - (float)(g_r[0x4D] & 0xFFFFu) / 16.0f;
    }
    /* Coordenadas en espacio PSP (480x272): el mapeo a cada pantalla vive en
     * la matriz ortográfica del target (superior o inferior). */
    o->x = sx;
    o->y = sy;
    o->z = 0.5f;
    if (g_magenta) {
        o->r = 1.0f; o->g = 0.0f; o->b = 1.0f; o->a = 1.0f;
    } else {
        o->r = (col & 255u) / 255.0f;
        o->g = ((col >> 8) & 255u) / 255.0f;
        o->b = ((col >> 16) & 255u) / 255.0f;
        o->a = ((col >> 24) & 255u) / 255.0f;
    }
    return 1;
}

/* ---- GPU texture cache ---------------------------------------------------
 * Minimal bounded proof-of-texture implementation. Cache keys include PSP GE
 * texture registers. Upload once per frame (guest RAM may be updated by CPU).
 * Never evict a texture referenced by the current, not-yet-finished frame.
 */
static void texture_release(GpuTexSlot *t) {
    if (!t->valid) return;
    C3D_TexDelete(&t->tex);
    if (g_tex_bytes >= t->bytes) g_tex_bytes -= t->bytes;
    memset(t, 0, sizeof(*t));
}
/* Huella muestreada del contenido: si no cambia, no se vuelve a convertir ni
 * subir (en hardware real la conversión por frame de 40+ texturas no da). */
static uint32_t tex_fingerprint(const uint8_t *src, uint32_t srcbytes,
                                const uint8_t *clut, uint32_t clutbytes) {
    uint32_t h = 2166136261u ^ srcbytes;
    if (srcbytes >= 4u) {
        for (uint32_t i = 0; i < 24u; i++) {
            const uint32_t off = (uint32_t)(((uint64_t)(srcbytes - 4u)) * i / 23u) & ~3u;
            uint32_t v; memcpy(&v, src + off, 4);
            h = (h ^ v) * 16777619u;
        }
    }
    if (clut)
        for (uint32_t i = 0; i + 4u <= clutbytes; i += 32u) {
            uint32_t v; memcpy(&v, clut + i, 4);
            h = (h ^ v) * 16777619u;
        }
    return h;
}
static int texture_upload(GpuTexSlot *t) {
    const uint32_t w = t->w, h = t->h;
    const int bpp = ge_tex_bpp((int)t->psm);
    if (bpp <= 0 || w < 8 || h < 8 || w > 1024 || h > 1024) return 0;
    const uint32_t pitch = t->stride ? t->stride : w;
    if (pitch < w || pitch > 2048) return 0;
    const uint32_t rows = t->swizzle ? ((h + 7u) & ~7u) : h;
    const uint32_t pitch_bytes = (pitch * (uint32_t)bpp + 7u) / 8u;
    const uint32_t srcbytes = (t->psm >= GE_PSM_DXT1) ?
        ((w + 3u) / 4u) * ((h + 3u) / 4u) * (t->psm == GE_PSM_DXT1 ? 8u : 16u) :
        (t->swizzle ? ((pitch_bytes + 15u) & ~15u) : pitch_bytes) * rows;
    const uint8_t *src = (const uint8_t *)psp_mem_ptr(t->addr, srcbytes);
    if (!src) return 0;
    const uint8_t *clut = NULL;
    const int indexed = (t->psm >= GE_PSM_T4 && t->psm <= GE_PSM_T32);
    const uint32_t cfmt = t->clut_cfg & 3u;
    const uint32_t cstart = (t->clut_cfg >> 16) & 31u;
    const uint32_t pal_count = (t->psm == GE_PSM_T4 ? 16u : 256u) + cstart * 16u;
    if (indexed) {
        if (t->psm > GE_PSM_T8) return 0;
        clut = (const uint8_t *)psp_mem_ptr(t->clut_addr,
                pal_count * (cfmt == GE_PSM_8888 ? 4u : 2u));
        if (!clut) return 0;
    }
    const uint32_t clutbytes = indexed ? pal_count * (cfmt == GE_PSM_8888 ? 4u : 2u) : 0u;
    const uint32_t fp = tex_fingerprint(src, srcbytes, clut, clutbytes);
    if (t->fp_valid && t->fp == fp) {       /* contenido sin cambios: no re-subir */
        t->frame_loaded = S.frames;
        g_tex_hits++;
        return 1;
    }
    ge_tex_image img = {0};
    if (ge_tex_decode(src, (int)w, (int)h, (int)t->psm, pitch, clut, (int)cfmt,
        (int)((t->clut_cfg >> 2) & 31u), (int)((t->clut_cfg >> 8) & 255u),
        (int)cstart, (int)t->swizzle, &img) != 0) {
        return 0;
    }
    {   /* TEMP [texdump]: con sdmc:/djmx_texdump.flag vuelca cada textura única
         * (arte intacto, antes del colorkey) a sdmc:/ge/tex/ para extraccion. */
        static int want = -1;
        if (want < 0) {
            FILE *f = fopen("sdmc:/djmx_texdump.flag", "rb");
            want = f != NULL;
            if (f) { fclose(f); mkdir("sdmc:/ge", 0777); mkdir("sdmc:/ge/tex", 0777); }
        }
        if (want) {
            static uint32_t seen[512]; static int nseen; static int files;
            int dup = 0;
            for (int i = 0; i < nseen; i++) if (seen[i] == fp) { dup = 1; break; }
            if (!dup && nseen < 512 && files < 400) {
                seen[nseen++] = fp;
                char path[96];
                snprintf(path, sizeof path, "sdmc:/ge/tex/%08X_%ux%u_%08X.png",
                         t->addr, w, h, fp);
                if (ge_tex_dump_png(path, &img) == 0) files++;
            }
        }
    }
    {   /* Texture packs: <FP>.rgba (RGBA8 crudo, WxH exactos) reemplaza el arte
         * decodificado. Fuentes: sdmc:/djmax/tex (el usuario manda) y romfs:/tex
         * (incluido en el CIA). El índice se arma una vez: sin I/O por cada miss. */
        static struct { uint32_t fp; uint8_t src; } packs[256];
        static int npacks, scanned;
        if (!scanned) {
            scanned = 1;
            static const char *const dirs[2] = { "sdmc:/djmax/tex", "romfs:/tex" };
            for (int s = 0; s < 2; s++) {
                DIR *d = opendir(dirs[s]);
                if (!d) continue;
                for (struct dirent *e; (e = readdir(d)) != NULL && npacks < 256; ) {
                    unsigned v;
                    if (strlen(e->d_name) == 13 && strcmp(e->d_name + 8, ".rgba") == 0 &&
                        sscanf(e->d_name, "%8x", &v) == 1) {
                        packs[npacks].fp = v; packs[npacks].src = (uint8_t)s; npacks++;
                    }
                }
                closedir(d);
            }
            psp_printf("[tex] packs indexados: %d\n", npacks);
        }
        for (int i = 0; i < npacks; i++) {
            if (packs[i].fp != fp) continue;
            char path[80];
            snprintf(path, sizeof path, packs[i].src ? "romfs:/tex/%08X.rgba" : "sdmc:/djmax/tex/%08X.rgba", fp);
            FILE *f = fopen(path, "rb");
            if (f) {
                const size_t need = (size_t)w * h * 4u;
                if (fread(img.rgba, 1, need, f) == need) {
                    static uint32_t ov_logs;
                    if (ov_logs < 8u) { ov_logs++;
                        psp_printf("[tex] override %08X (%ux%u) %s\n", fp, w, h,
                                   packs[i].src ? "romfs" : "sdmc"); }
                }
                fclose(f);
            }
            break;
        }
    }
    /* PSP color test (GU_NOTEQUAL): los texels que casan con la clave no se
     * dibujan. PICA no tiene color-key: se vacían en la textura (RGBA=0, que
     * también anula blends aditivos). */
    if (t->ckey != 0xFFFFFFFFu) {
        const uint32_t key = t->ckey & t->cmask & 0xFFFFFFu;
        for (uint32_t i = 0; i < w * h; i++)
            if ((img.rgba[i] & t->cmask & 0xFFFFFFu) == key) img.rgba[i] = 0;
    }
    /* DJMAX marca recortes con magenta puro en skins del gear (la PSP los
     * tapa con draws posteriores); FF00FF exacto pasa a transparente. */
    for (uint32_t i = 0; i < w * h; i++)
        if ((img.rgba[i] & 0x00FFFFFFu) == 0x00FF00FFu) img.rgba[i] = 0;
    uint8_t *pica = (uint8_t *)malloc((size_t)w * h * 4u);
    if (!pica) { ge_tex_free(&img); return 0; }
    ge_tex_to_pica_rgba8(&img, pica);
    C3D_TexUpload(&t->tex, pica);
    C3D_TexFlush(&t->tex);
    free(pica);
    ge_tex_free(&img);
    t->fp = fp;
    if (t->fp_valid) t->last_change = S.frames;   /* contenido cambió: textura de video */
    t->fp_valid = 1;
    t->frame_loaded = S.frames;
    g_tex_uploads++;
    return 1;
}
static GpuTexSlot *texture_get(void) {
    const uint32_t addr = (g_r[0xA0] & 0xFFFFFFu) | ((g_r[0xA8] & 0x0F0000u) << 8);
    const uint32_t w = 1u << (g_r[0xB8] & 15u);
    const uint32_t h = 1u << ((g_r[0xB8] >> 8) & 15u);
    const uint32_t psm = g_r[0xC3] & 15u;
    const uint32_t stride = g_r[0xA8] & 0xFFFFu;
    const uint32_t clut_addr = (g_r[0xB0] & 0xFFFFFFu) | ((g_r[0xB1] & 0x0F0000u) << 8);
    const uint32_t cfg = g_r[0xC5];
    const uint32_t sw = g_r[0xC2] & 1u;
    /* color test NOTEQUAL activo: clave de recorte por color (0x23/0xE2-0xE4) */
    uint32_t ckey = 0xFFFFFFFFu, cmask = 0xFFFFFFu;
    if ((g_r[0x23] & 1u) && !(g_r[0xD3] & 1u) && (g_r[0xE2] & 3u) == 3u) {
        ckey = g_r[0xE3] & 0xFFFFFFu;
        cmask = g_r[0xE4] & 0xFFFFFFu;
        static uint32_t ck_logs;
        if (ck_logs < 4u) { ck_logs++;
            psp_printf("[ctest] key=%06X mask=%06X\n", ckey, cmask); }
    }
    if (psm > GE_PSM_DXT5 || w < 8 || h < 8 || w > 1024 || h > 1024) {
        static uint32_t rej_logs;
        if (rej_logs < 8) { rej_logs++;
            psp_printf("[tex] REJ addr=0x%08X %ux%u psm=%u stride=%u\n", addr, w, h, psm, stride); }
        return NULL;
    }
    const uint32_t bytes = w * h * 4u;
    if (bytes > GE_TEXTURE_BUDGET) return NULL;
    GpuTexSlot *slot = NULL;
    for (int i = 0; i < GE_TEXTURE_SLOTS; i++) {
        GpuTexSlot *t = &g_texcache[i];
        if (t->valid && t->addr == addr && t->w == w && t->h == h && t->psm == psm &&
            t->stride == stride && t->clut_addr == clut_addr &&
            t->clut_cfg == cfg && t->swizzle == sw &&
            t->ckey == ckey && t->cmask == cmask) {
            slot = t;
            g_tex_hits++;
            break;
        }
    }
    if (!slot) {
        for (int i = 0; i < GE_TEXTURE_SLOTS; i++)
            if (!g_texcache[i].valid) { slot = &g_texcache[i]; break; }
        if (!slot) {
            /* A slot is only safe to reuse if no draw this frame references it. */
            for (int i = 0; i < GE_TEXTURE_SLOTS; i++)
                if (g_texcache[i].frame_used != S.frames) { slot = &g_texcache[i]; break; }
        }
        if (!slot) {
            static uint32_t noslot_logs;
            if (noslot_logs < 6) { noslot_logs++;
                psp_printf("[tex] sin slot: %u slots en uso este frame (addr=0x%08X %ux%u)\n",
                           (unsigned)GE_TEXTURE_SLOTS, addr, w, h); }
            return NULL;
        }
        texture_release(slot);
        while (g_tex_bytes + bytes > GE_TEXTURE_BUDGET) {
            GpuTexSlot *victim = NULL;
            for (int i = 0; i < GE_TEXTURE_SLOTS; i++)
                if (g_texcache[i].valid && g_texcache[i].frame_used != S.frames &&
                    &g_texcache[i] != slot) { victim = &g_texcache[i]; break; }
            if (!victim) {
                static uint32_t ev_logs;
                if (ev_logs < 6) { ev_logs++;
                    psp_printf("[tex] sin victima: bytes=%u + %u > budget (addr=0x%08X %ux%u)\n",
                               g_tex_bytes, bytes, addr, w, h); }
                return NULL;
            }
            texture_release(victim);
        }
        if (!C3D_TexInit(&slot->tex, (u16)w, (u16)h, GPU_RGBA8)) {
            static uint32_t ti_logs;
            if (ti_logs < 6) { ti_logs++;
                psp_printf("[tex] C3D_TexInit FALLO %ux%u (lineal agotada? bytes=%u usados=%u)\n",
                           w, h, bytes, g_tex_bytes); }
            return NULL;
        }
        C3D_TexSetFilter(&slot->tex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&slot->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        slot->addr = addr; slot->w = w; slot->h = h; slot->psm = psm;
        slot->stride = stride; slot->clut_addr = clut_addr;
        slot->clut_cfg = cfg; slot->swizzle = sw;
        slot->ckey = ckey; slot->cmask = cmask;
        slot->bytes = bytes; slot->valid = 1;
        slot->fp_valid = 0;
        g_tex_bytes += bytes;
        /* Force initial upload; frame_used makes this safe on subsequent draws. */
        slot->frame_loaded = UINT32_MAX;
    }
    if (slot->frame_loaded != S.frames && !texture_upload(slot)) {
        g_tex_fail++;
        return NULL;
    }
    slot->frame_used = S.frames;
    return slot;
}

/* ---- draw buffer ---------------------------------------------------------- */
static const void *g_last_rt;     /* evita re-bind del target por cada batch */
static float g_slider;            /* slider 3D leído al abrir el frame */
static void flush(void) {
    if (!g_vn || !g_ready || !g_frame_open) { g_vn = 0; return; }
    const int stereo = !g_route && g_right && g_slider > 0.01f && g_cur_vp;
    C3D_Mtx ml, mr;
    const C3D_Mtx *mx = g_cur_mtx ? g_cur_mtx : &g_mtx;
    if (stereo) {
        /* Disparidad p = x_der - x_izq (unidades PSP; >0 = detrás del plano de
         * pantalla). Cada ojo ve la escena corrida p/2: mover la ventana a la
         * derecha corre el contenido a la izquierda. */
        const float h = 0.5f * g_cur_depth * g_slider;
        Mtx_OrthoTilt(&ml, g_cur_vp->l + h, g_cur_vp->r + h, g_cur_vp->b, g_cur_vp->t, 0.0f, 1.0f, true);
        Mtx_OrthoTilt(&mr, g_cur_vp->l - h, g_cur_vp->r - h, g_cur_vp->b, g_cur_vp->t, 0.0f, 1.0f, true);
        mx = &ml;
    }
    C3D_RenderTarget *rt = (C3D_RenderTarget *)((g_route && g_bottom) ? g_bottom : g_target);
    if ((const void *)rt != g_last_rt) {
        C3D_FrameDrawOn(rt);
        g_last_rt = rt;
    }
    C3D_BufInfo *bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, g_vb, sizeof(Vtx), 3, 0x210);
    GSPGPU_FlushDataCache(&g_vb[g_vbase], (size_t)g_vn * sizeof(Vtx));
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_uproj, mx);
    C3D_DrawArrays(GPU_TRIANGLES, g_vbase, g_vn);
    if (stereo) {
        C3D_FrameDrawOn((C3D_RenderTarget *)g_right);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_uproj, &mr);
        C3D_DrawArrays(GPU_TRIANGLES, g_vbase, g_vn);
        g_last_rt = g_right;
    }
    g_vbase += g_vn;
    g_vn = 0;
}
static void push(const Vtx *v) {
    if (g_vbase + g_vn >= VB_VERTS) { g_draw_drop++; return; }
    g_vb[g_vbase + g_vn++] = *v;
}
static void tri(const Vtx *a, const Vtx *b, const Vtx *c) { push(a); push(b); push(c); }
/* ---- user-asset fallback --------------------------------------------------
 * This is a diagnostic preview of PSP_GAME/PIC1.PNG/ICON0.PNG, NOT the game renderer.
 * The real GE geometry always has priority; preview only when the game
 * submitted no vertices during the just-completed frame.
 */
static void preview_init(void) {
    FILE *flag = fopen("sdmc:/djmax/force_preview.flag", "rb");
    if (flag) { g_force_preview = 1; fclose(flag); }
    FILE *f = fopen("sdmc:/djmax/preview.rgba", "rb");
    if (!f) {
        psp_printf("[asset-preview] preview.rgba missing; run CONFIGURAR_ASSETS_3DS.bat\n");
        return;
    }
    const size_t bytes = PREVIEW_W * PREVIEW_H * 4u;
    uint32_t *linear = (uint32_t *)malloc(bytes);
    uint8_t *tiled = (uint8_t *)malloc(bytes);
    if (!linear || !tiled || fread(linear, 1, bytes, f) != bytes) {
        psp_printf("[asset-preview] read/alloc FAILED\n");
        fclose(f); free(linear); free(tiled); return;
    }
    fclose(f);
    ge_tex_image img = {0};
    img.rgba = linear; img.w = (int)PREVIEW_W; img.h = (int)PREVIEW_H;
    ge_tex_to_pica_rgba8(&img, tiled);
    if (C3D_TexInit(&g_preview_tex, PREVIEW_W, PREVIEW_H, GPU_RGBA8)) {
        C3D_TexUpload(&g_preview_tex, tiled);
        C3D_TexFlush(&g_preview_tex);
        C3D_TexSetFilter(&g_preview_tex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&g_preview_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        g_preview_ready = 1;
        psp_printf("[asset-preview] PIC1/ICON0 converted asset uploaded (256x128); diagnostic only\n");
    } else psp_printf("[asset-preview] C3D_TexInit FAILED\n");
    free(linear); free(tiled);
}
static void preview_draw(void) {
    if (!g_preview_ready || !g_ready || !g_frame_open) return;
    flush();
    C3D_TexBind(0, &g_preview_tex);
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    /* Distinct inset: evidence that the real user asset is on the SD, not a
     * fabricated game frame. 16:8 aspect ratio in centre of top display. */
    const float x0=48.0f, x1=352.0f, y0=47.0f, y1=199.0f;
    const Vtx a = {x0,y0,0.5f, 0,1, 1,1,1,1};
    const Vtx b = {x1,y0,0.5f, 1,1, 1,1,1,1};
    const Vtx c = {x1,y1,0.5f, 1,0, 1,1,1,1};
    const Vtx d = {x0,y1,0.5f, 0,0, 1,1,1,1};
    tri(&a,&b,&c); tri(&a,&c,&d);
    g_preview_frames++;
    if (g_preview_frames == 1u)
        psp_printf("[asset-preview] displayed real PIC1/ICON0 asset; GE game image not yet confirmed\n");
}



/* ---- frame lifecycle ------------------------------------------------------ */
void ps2_sched_yield(void);
static void frame_begin(void) {
    if (g_frame_open || !g_ready || !g_target) return;
    g_vbase = g_vn = 0;
    g_route = 0;
    g_cur_mtx = &g_mtx;
    g_cur_vp = &g_vp_top;
    g_cur_depth = 3.0f;
    /* SYNCDRAW bloquearía el host ~16 ms (GPU+vblank) matando de hambre a los
     * hilos guest (granos SAS de 5.8 ms). Como el PSP real: la CPU sigue
     * corriendo mientras el GE dibuja. */
    while (!C3D_FrameBegin(C3D_FRAME_NONBLOCK)) {
        ps2_sched_yield();
        svcSleepThread(100000);        /* 0.1 ms si nadie más está listo */
    }
    C3D_RenderTargetClear((C3D_RenderTarget *)g_target, C3D_CLEAR_ALL, g_clear, 0);
    g_slider = g_right ? osGet3DSliderState() : 0.0f;
    if (g_right) {   /* modo 3D de la consola solo mientras el slider está arriba */
        static int is3d = -1;
        const int want = g_slider > 0.01f;
        if (want != is3d) { is3d = want; gfxSet3D(want); }
    }
    if (g_right && g_slider > 0.01f)
        C3D_RenderTargetClear((C3D_RenderTarget *)g_right, C3D_CLEAR_ALL, g_clear, 0);
    if (g_bottom)
        C3D_RenderTargetClear((C3D_RenderTarget *)g_bottom, C3D_CLEAR_ALL, 0x000000FFu, 0);
    C3D_FrameDrawOn((C3D_RenderTarget *)g_target);
    g_last_rt = g_target;
    g_frame_open = 1;
}
static void frame_end(void) {
    if (!g_frame_open) return;
    flush();
    C3D_FrameEnd(0);
    g_frame_open = 0;
}
/* Draw original ICON0 before starting PSP guest threads; it remains visible
 * even when the game hangs before its first sceDisplaySetFrameBuf. */
void ge_draw_preview_boot(void) {
    if (!g_preview_ready || !g_ready || !g_target) return;
    frame_begin(); preview_draw(); frame_end();
}

/* A guest that never reaches sceDisplaySetFrameBuf can leave the first
 * diagnostic image hidden behind an emulator boot frame. Explicit refresh
 * is used ONLY while force_preview.flag is present, not during real GE play. */
int ge_draw_preview_forced(void) { return g_force_preview && g_preview_ready && g_ready; }
void ge_draw_preview_refresh(void) {
    if (!ge_draw_preview_forced() || !g_target) return;
    if (g_frame_open) return; /* Never interrupt a guest's in-flight draw list. */
    frame_begin();
    preview_draw();
    frame_end();
}

/* ---- PRIM ----------------------------------------------------------------- */
static void census_add(const ge_census_call *c) {
    if (g_census_n < GE_CENSUS_MAX) g_census[g_census_n++] = *c;
}

static void prim_sprites(const Vtx *v, uint32_t n, const ge_census_call *cc) {
    /* Medio texel de margen hacia dentro: el reescalado 480->400 muestrea en
     * los bordes del rect y sangra texels vecinos del atlas (franjas magenta). */
    const float iu = cc->tex_on && cc->tex_w ? 0.5f / (float)cc->tex_w : 0.0f;
    const float iv = cc->tex_on && cc->tex_h ? 0.5f / (float)cc->tex_h : 0.0f;
    for (uint32_t i = 0; i + 1u < n; i += 2u) {
        const Vtx *a = &v[i], *b = &v[i + 1u];
        const float x0 = a->x < b->x ? a->x : b->x, x1 = a->x < b->x ? b->x : a->x;
        const float y0 = a->y < b->y ? a->y : b->y, y1 = a->y < b->y ? b->y : a->y;
        float u0 = a->x < b->x ? a->u : b->u, u1 = a->x < b->x ? b->u : a->u;
        float v0 = a->y < b->y ? a->v : b->v, v1 = a->y < b->y ? b->v : a->v;
        if (u1 - u0 > 2.0f * iu)      { u0 += iu; u1 -= iu; }
        else if (u0 - u1 > 2.0f * iu) { u0 -= iu; u1 += iu; }
        if (v1 - v0 > 2.0f * iv)      { v0 += iv; v1 -= iv; }
        else if (v0 - v1 > 2.0f * iv) { v0 -= iv; v1 += iv; }
        Vtx q[4];
        q[0] = (Vtx){ x0, y0, a->z, u0, v0, a->r, a->g, a->b, a->a };
        q[1] = (Vtx){ x1, y0, a->z, u1, v0, b->r, b->g, b->b, b->a };
        q[2] = (Vtx){ x1, y1, a->z, u1, v1, b->r, b->g, b->b, b->a };
        q[3] = (Vtx){ x0, y1, a->z, u0, v1, a->r, a->g, a->b, a->a };
        tri(&q[0], &q[1], &q[2]);
        tri(&q[0], &q[2], &q[3]);
    }
}

static void prim_tris(const Vtx *v, uint32_t n, uint32_t type) {
    switch (type) {
    case 3: /* triangles */
        for (uint32_t i = 0; i + 2u < n; i += 3u) tri(&v[i], &v[i+1u], &v[i+2u]);
        break;
    case 4: /* strip */
        for (uint32_t i = 0; i + 2u < n; i++) {
            if ((i & 1u) == 0u) tri(&v[i], &v[i+1u], &v[i+2u]);
            else                tri(&v[i+1u], &v[i], &v[i+2u]);
        }
        break;
    case 5: /* fan */
        for (uint32_t i = 1; i + 1u < n; i++) tri(&v[0], &v[i], &v[i+1u]);
        break;
    default: break;
    }
}

/* PSP GE advances its source cursor even when a draw is not renderable.
 * Keep this independent of whether PICA200 accepted the primitive. */
static void prim_advance(uint32_t itype, uint32_t count, uint32_t stride) {
    /* Index streams advance by indices, not by decoded vertex stride. */
    if (itype == 1u) g_iaddr += count;
    else if (itype == 2u) g_iaddr += count * 2u;
    else if (itype == 3u) g_iaddr += count * 4u;
    else g_vaddr += stride * count;
}

/* GE blend factor (0xDF bits) to PICA. 'fix' is SFIX/DFIX (0xE0/0xE1) when f==10.
 * Factors 6-9 (2x alpha) have no PICA form; the 1x alpha is the nearest. */
static GPU_BLENDFACTOR ge_blend_factor(uint32_t f, int is_src, uint32_t fix) {
    switch (f) {
    case 0:  return is_src ? GPU_DST_COLOR : GPU_SRC_COLOR;
    case 1:  return is_src ? GPU_ONE_MINUS_DST_COLOR : GPU_ONE_MINUS_SRC_COLOR;
    case 2: case 6:  return GPU_SRC_ALPHA;
    case 3: case 7:  return GPU_ONE_MINUS_SRC_ALPHA;
    case 4: case 8:  return GPU_DST_ALPHA;
    case 5: case 9:  return GPU_ONE_MINUS_DST_ALPHA;
    default:
        if (fix >= 0xF0F0F0u) return GPU_ONE;
        if (fix == 0u)        return GPU_ZERO;
        return GPU_CONSTANT_COLOR;
    }
}
/* GE 0x21 blend enable, 0xDF mode, 0xE0/0xE1 fixed colours, 0xD3 clear mode. */
static void set_blend(void) {
    if (!(g_r[0x21] & 1u) || (g_r[0xD3] & 1u)) {
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        return;
    }
    const uint32_t m = g_r[0xDF];
    const uint32_t sfix = g_r[0xE0] & 0xFFFFFFu, dfix = g_r[0xE1] & 0xFFFFFFu;
    static const GPU_BLENDEQUATION eq[8] = {
        GPU_BLEND_ADD, GPU_BLEND_SUBTRACT, GPU_BLEND_REVERSE_SUBTRACT,
        GPU_BLEND_MIN, GPU_BLEND_MAX, GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_BLEND_ADD };
    GPU_BLENDFACTOR s = ge_blend_factor(m & 15u, 1, sfix);
    GPU_BLENDFACTOR d = ge_blend_factor((m >> 4) & 15u, 0, dfix);
    /* PICA tiene UNA constante de blend. Caso clásico PSP: ambos FIX con
     * dfix ≈ ~sfix (fundido k/1-k): constante=sfix y el destino usa 1-cte. */
    if (s == GPU_CONSTANT_COLOR && d == GPU_CONSTANT_COLOR) {
        const uint32_t sum = (sfix & 255u) + (dfix & 255u);
        if (sum >= 0xF0u && sum <= 0x10Fu) d = GPU_ONE_MINUS_CONSTANT_COLOR;
        C3D_BlendingColor(sfix | 0xFF000000u);
    } else if (s == GPU_CONSTANT_COLOR) {
        C3D_BlendingColor(sfix | 0xFF000000u);
    } else if (d == GPU_CONSTANT_COLOR) {
        C3D_BlendingColor(dfix | 0xFF000000u);
    }
    C3D_AlphaBlend(eq[(m >> 8) & 7u], GPU_BLEND_ADD, s, d, GPU_ONE, GPU_ZERO);
}
/* GE 0x22 alpha-test enable, 0xDB func/ref. */
static void set_alpha_test(void) {
    if ((g_r[0x22] & 1u) && !(g_r[0xD3] & 1u)) {
        static const GPU_TESTFUNC fn[8] = {
            GPU_NEVER, GPU_ALWAYS, GPU_EQUAL, GPU_NOTEQUAL,
            GPU_LESS, GPU_LEQUAL, GPU_GREATER, GPU_GEQUAL };
        C3D_AlphaTest(true, fn[g_r[0xDB] & 7u], (int)((g_r[0xDB] >> 8) & 255u));
    } else {
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
    }
}

static void prim_run(uint32_t prim_pc, uint32_t arg) {
    const uint32_t type = (arg >> 16) & 7u;
    const uint32_t count = arg & 0xFFFFu;
    const uint32_t vt = g_vtype;
    const uint32_t itype = (vt >> 11) & 3u;
    const uint32_t pos_type = (vt >> 7) & 3u;
    const uint32_t stride = vertex_size(vt);

    ge_census_call cc;
    memset(&cc, 0, sizeof cc);
    cc.list_pc = prim_pc; cc.prim_pc = prim_pc; cc.type = type; cc.count = count;
    cc.vtype = vt; cc.pos_type = pos_type; cc.uv_type = vt & 3u;
    cc.color_type = (vt >> 2) & 7u; cc.index_type = itype; cc.through_2d = (vt >> 23) & 1u;
    cc.vaddr = g_vaddr; cc.stride = stride;
    cc.tex_on = g_r[0x1E] & 1u;
    cc.tex_addr = (g_r[0xA0] & 0xFFFFFFu) | ((g_r[0xA8] & 0xFF0000u) << 8);
    cc.tex_stride = g_r[0xA8] & 0xFFFFu;
    cc.tex_w = 1u << (g_r[0xB8] & 15u);
    cc.tex_h = 1u << ((g_r[0xB8] >> 8) & 15u);
    cc.tex_psm = g_r[0xC3] & 15u;
    cc.bbox[0] = 0x7FFFFFFF; cc.bbox[1] = 0x7FFFFFFF;

    S.prims++;
    cc.status = "SUBMITTED";
    if (pos_type == 0u) { S.no_pos++; cc.status = "NO_POSITION"; census_add(&cc); prim_advance(itype, count, stride); return; }
    if (stride == 0u)   { S.other++;  cc.status = "BAD_STRIDE"; census_add(&cc); prim_advance(itype, count, stride); return; }
    if (count < 2u)     { S.other++;  cc.status = "SHORT"; census_add(&cc); prim_advance(itype, count, stride); return; }

    /* address/bounds: decode without trusting the guest pointer */
    uint32_t idx_bytes = 0;
    if (itype == 1u) idx_bytes = count;
    else if (itype == 2u) idx_bytes = count * 2u;
    else if (itype == 3u) idx_bytes = count * 4u;
    /* Indexed GE draws address vertices independently of the index stream.
     * Checking only the index array does not validate the actual vertex data.
     * Use 64-bit arithmetic to reject wraparound before touching guest RAM. */
    const uint64_t stream_bytes = itype ? (uint64_t)idx_bytes : (uint64_t)stride * count;
    const uint32_t stream_addr = itype ? g_iaddr : g_vaddr;
    if (!stream_bytes || stream_bytes > UINT32_MAX ||
        (uint64_t)stream_addr + stream_bytes > UINT32_MAX ||
        !psp_mem_ptr(stream_addr, (uint32_t)stream_bytes)) {
        S.bad_addr++; cc.status = "BAD_ADDR"; census_add(&cc); prim_advance(itype, count, stride); return;
    }
    if (itype) {
        uint32_t max_index = 0;
        const uint32_t inspected = count > 1024u ? 1024u : count;
        for (uint32_t i = 0; i < inspected; ++i) {
            const uint32_t idx = itype == 1u ? psp_read8(g_iaddr + i) :
                itype == 2u ? psp_read16(g_iaddr + i * 2u) :
                              psp_read32(g_iaddr + i * 4u);
            if (idx > max_index) max_index = idx;
        }
        const uint64_t vertex_bytes = ((uint64_t)max_index + 1u) * stride;
        if (!vertex_bytes || vertex_bytes > UINT32_MAX ||
            (uint64_t)g_vaddr + vertex_bytes > UINT32_MAX ||
            !psp_mem_ptr(g_vaddr, (uint32_t)vertex_bytes)) {
            S.bad_addr++; cc.status = "BAD_INDEXED_VERTEX_ADDR";
            census_add(&cc); prim_advance(itype, count, stride); return;
        }
    }

    frame_begin();
    /* Per-primitive state. Flush without reusing the GPU's pending vertex data. */
    flush();
    {   /* TEMP [blend]: primeras apariciones de factores FIX (gear/transparencias) */
        const uint32_t m = g_r[0xDF];
        if ((g_r[0x21] & 1u) && (((m & 15u) == 10u) || (((m >> 4) & 15u) == 10u))) {
            static uint32_t bl;
            if (bl < 20u) { bl++;
                psp_printf("[blend] f=%u tex=%u tf=0x%X m=0x%06X sfix=%06X dfix=%06X at=%u db=0x%X n=%u\n",
                           S.frames, cc.tex_on, g_r[0xC9], m,
                           g_r[0xE0] & 0xFFFFFFu, g_r[0xE1] & 0xFFFFFFu,
                           g_r[0x22] & 1u, g_r[0xDB], count); }
        }
    }
    GpuTexSlot *texture = NULL;
    /* Descriptor degenerado (addr 0 / 1x1): el juego deja tex habilitado sin
     * textura real; es geometría de color plano, no un fallo. */
    const int tex_degenerate = cc.tex_on &&
        (cc.tex_addr == 0u || cc.tex_w < 8u || cc.tex_h < 8u);
    if (!g_magenta && cc.tex_on && !tex_degenerate) texture = texture_get();
    if (!texture && cc.tex_on && !tex_degenerate && !g_magenta) {
        static uint32_t miss_logs;
        if (miss_logs < 10) { miss_logs++;
            psp_printf("[tex] MISS addr=0x%08X %ux%u psm=%u stride=%u clut=0x%08X\n",
                       cc.tex_addr, cc.tex_w, cc.tex_h, cc.tex_psm, cc.tex_stride,
                       (g_r[0xB0] & 0xFFFFFFu) | ((g_r[0xB1] & 0x0F0000u) << 8)); }
    }
    /* Dual-screen: la ruta se decide tras leer los vértices (ver bbox abajo). */
    if (texture) C3D_TexBind(0, &texture->tex);
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    if (texture) {
        /* PSP TFUNC 0xC9: bits 0-2 function, bit 8 TCC (use texture alpha),
         * bit 16 colour doubling. Modulate against the per-vertex colour. */
        const uint32_t tf = g_r[0xC9];
        switch (tf & 7u) {
        case 3:  /* replace */
            C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, 0, 0);
            C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
            break;
        case 4:  /* add */
            C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
            C3D_TexEnvFunc(env, C3D_RGB, GPU_ADD);
            break;
        default: /* modulate / decal / blend */
            C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
            C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
            break;
        }
        if ((tf >> 8) & 1u) {
            C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
            C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
        } else {
            C3D_TexEnvSrc(env, C3D_Alpha, GPU_PRIMARY_COLOR, 0, 0);
            C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
        }
        C3D_TexEnvScale(env, C3D_RGB, (tf >> 16) & 1u ? GPU_TEVSCALE_2 : GPU_TEVSCALE_1);
    } else if (g_magenta) {
        /* Texturing requested but no usable upload: loud diagnostic magenta. */
        C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, 0, 0);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        C3D_TexEnvColor(env, 0xFFFF00FFu);
    } else {
        C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    }
    set_blend();
    set_alpha_test();

    static Vtx tmp[1024];
    uint32_t n = count > 1024u ? 1024u : count;
    uint32_t got = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = i;
        if (itype == 1u)      idx = psp_read8(g_iaddr + i);
        else if (itype == 2u) idx = psp_read16(g_iaddr + i * 2u);
        else if (itype == 3u) idx = psp_read32(g_iaddr + i * 4u);
        int has_uv = 0;
        Vtx v;
        /* Guard each indexed address, including indices beyond the first
         * 1024 if future builds increase the temporary vertex capacity. */
        const uint64_t vertex_addr = (uint64_t)g_vaddr + (uint64_t)idx * stride;
        if (vertex_addr > UINT32_MAX ||
            !psp_mem_ptr((uint32_t)vertex_addr, stride) ||
            !vtx_read((uint32_t)vertex_addr, vt, &v, &has_uv)) continue;
        if (!isfinite(v.x) || !isfinite(v.y) || !isfinite(v.z) ||
            !isfinite(v.u) || !isfinite(v.v)) continue;
        if (texture && has_uv) {
            if (cc.through_2d) { v.u /= (float)texture->w; v.v /= (float)texture->h; }
            v.v = 1.0f - v.v;
        }
        tmp[got++] = v;
        const int sx = (int)v.x, sy = (int)v.y;
        if (sx < cc.bbox[0]) cc.bbox[0] = sx;
        if (sy < cc.bbox[1]) cc.bbox[1] = sy;
        if (sx > cc.bbox[2]) cc.bbox[2] = sx;
        if (sy > cc.bbox[3]) cc.bbox[3] = sy;
    }
    if (cc.bbox[0] == 0x7FFFFFFF) { cc.bbox[0] = cc.bbox[1] = cc.bbox[2] = cc.bbox[3] = 0; }
    /* Gameplay: el chasis del gear es el primer draw alto (≥230) y angosto
     * (≤340) del frame. El BGA se dibuja antes (arriba); gear/notas/HUD van
     * después (abajo). El modo dual queda armado 60 frames. */
    if (g_bottom && !g_gear_seen && cc.tex_on && got >= 2u) {
        const int bw = cc.bbox[2] - cc.bbox[0], bh = cc.bbox[3] - cc.bbox[1];
        const int contained = cc.bbox[1] >= -8 && cc.bbox[3] <= 276;
        /* Firma real del gear ([cand]): telón sin textura (~143x211) centrado;
         * pivote de ruta y disparador del modo dual. */
        if (!texture && bh >= 180 && bw >= 80 && bw <= 260 && contained &&
            cc.bbox[0] <= 260 && cc.bbox[2] >= 220) {
            g_gear_seen = 1; g_split = 60;
            float cx = 0.5f * (float)(cc.bbox[0] + cc.bbox[2]);
            if (cx < 181.33f) cx = 181.33f;
            if (cx > 298.67f) cx = 298.67f;
            if (cx > g_split_cx + 4.0f || cx < g_split_cx - 4.0f) {
                g_split_cx = cx;
                g_vp_split.l = cx - 181.33f;     g_vp_split.r = cx + 181.33f;
                g_vp_split_top.l = cx - 226.67f; g_vp_split_top.r = cx + 226.67f;
                Mtx_OrthoTilt(&g_mtx_split, g_vp_split.l, g_vp_split.r,
                              PSP_H, 0.0f, 0.0f, 1.0f, true);
                Mtx_OrthoTilt(&g_mtx_split_top, g_vp_split_top.l, g_vp_split_top.r,
                              PSP_H, 0.0f, 0.0f, 1.0f, true);
            }
            static uint32_t det_logs;
            if (det_logs < 4u) { det_logs++;
                psp_printf("[dual] telon f=%u bb=%d,%d-%d,%d\n", S.frames,
                           cc.bbox[0], cc.bbox[1], cc.bbox[2], cc.bbox[3]); }
        }
    }
    /* Clase del draw: gear/HUD (tras el telón) o BGA (antes, o sprite grande
     * animado/lateral). SELECT elige en qué pantalla vive cada clase. */
    const int big_tex = texture && texture->w >= 256u;
    const int anim_bga = big_tex && texture->last_change &&
        (S.frames - texture->last_change) < 30u;
    const int lateral_bga = big_tex && (cc.bbox[2] - cc.bbox[0]) >= 60 &&
        (cc.bbox[3] - cc.bbox[1]) >= 140 &&
        (cc.bbox[0] > 260 || cc.bbox[2] < 220);
    if (g_bottom && g_split > 0) {
        const int is_gear = g_gear_seen && !anim_bga && !lateral_bga;
        switch (g_layout) {
        default: /* 0: BGA arriba, gear abajo */
            if (is_gear) { g_route = 1; g_cur_mtx = &g_mtx_split; g_cur_vp = &g_vp_split; g_cur_depth = 1.5f; }
            else         { g_route = 0; g_cur_mtx = &g_mtx;       g_cur_vp = &g_vp_top;   g_cur_depth = 6.0f; }
            break;
        case 1:  /* BGA abajo, gear arriba */
            if (is_gear) { g_route = 0; g_cur_mtx = &g_mtx_split_top; g_cur_vp = &g_vp_split_top; g_cur_depth = 1.5f; }
            else         { g_route = 1; g_cur_mtx = &g_mtx_bot;      g_cur_vp = &g_vp_bot;       g_cur_depth = 6.0f; }
            break;
        case 2:  /* todo abajo */
            g_route = 1; g_cur_mtx = &g_mtx_bot; g_cur_vp = &g_vp_bot;
            g_cur_depth = is_gear ? 1.5f : 6.0f;
            break;
        case 3:  /* todo arriba */
            g_route = 0; g_cur_mtx = &g_mtx; g_cur_vp = &g_vp_top;
            g_cur_depth = is_gear ? 1.5f : 6.0f;
            break;
        }
    } else {
        g_route = 0; g_cur_mtx = &g_mtx; g_cur_vp = &g_vp_top; g_cur_depth = 3.0f;
    }
    {   /* Capas de profundidad estéreo (disparidad en unidades PSP a slider=1,
         * >0 = detrás de la pantalla): lo grande es fondo, lo pequeño (HUD,
         * texto, notas) se acerca al plano; el gear sobresale un poco. */
        int bw = cc.bbox[2] - cc.bbox[0], bh = cc.bbox[3] - cc.bbox[1];
        if (bw < 0) bw = 0; else if (bw > 480) bw = 480;
        if (bh < 0) bh = 0; else if (bh > 272) bh = 272;
        const float area = (float)(bw * bh) / (480.0f * 272.0f);
        float d = area >= 0.55f ? 7.0f : area >= 0.15f ? 3.5f : 1.0f;
        if (g_bottom && g_split > 0 && g_gear_seen && !anim_bga && !lateral_bga)
            d = area >= 0.10f ? -1.0f : -2.5f;
        g_cur_depth = d;
    }
    if (got >= 2u) {
        /* Textura degenerada (addr 0): cristal ahumado del gear. Con el BGA en
         * la misma pantalla (layouts 2/3) es un velo translúcido sobre él; con
         * el BGA en la otra pantalla (0/1) el fondo del carril es oscuro sólido. */
        if (cc.tex_on && tex_degenerate) {
            const int solo = g_bottom && g_split > 0 && g_layout < 2;
            for (uint32_t i = 0; i < got; i++) {
                tmp[i].r = solo ? 0.05f : 0.0f;
                tmp[i].g = solo ? 0.05f : 0.0f;
                tmp[i].b = solo ? 0.07f : 0.0f;
                tmp[i].a = solo ? 1.0f : 0.72f;
            }
            S.submitted++;
            if (type == 6u) prim_sprites(tmp, got, &cc);
            else prim_tris(tmp, got, type);
            if (g_vn > 0) g_guest_vertices_this_frame += (uint32_t)g_vn;
        } else {
            S.submitted++;
            if (cc.tex_on) S.textured++;
            if (type == 6u) prim_sprites(tmp, got, &cc);
            else prim_tris(tmp, got, type);
            if (g_vn > 0) g_guest_vertices_this_frame += (uint32_t)g_vn;
        }
    } else {
        S.other++;
        cc.status = "NO_VERTS";
    }
    census_add(&cc);

    /* Keep accepted and rejected PRIM cursor semantics identical. */
    prim_advance(itype, count, stride);
}

/* ---- resumable PSP GE command-list interpreter --------------------------
 * Must preserve PC across sceGeListUpdateStallAddr calls. A linear 2048-word
 * scan counts random data as PRIM and misses commands after a stall.
 */
void ge_draw_start(ge_draw_list_state *s, uint32_t list, uint32_t stall) {
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->pc = list;
    s->stall = stall;
    S.lists++;
}
int ge_draw_resume(ge_draw_list_state *s) {
    if (!s || s->finished) return s && s->finished ? 1 : -1;
    uint32_t budget = LIST_BUDGET;
    while (budget--) {
        if (s->stall && ((s->pc & PSP_ADDR_MASK) == (s->stall & PSP_ADDR_MASK))) return 0;
        if (!psp_mem_ptr(s->pc, 4)) { S.lists_bad++; s->finished = 1; return -1; }
        const uint32_t w = psp_read32(s->pc);
        const uint32_t op = w >> 24, arg = w & 0xFFFFFFu;
        const uint32_t rel = (((g_base & 0x00FF0000u) << 8) | arg) + g_offset;
        s->pc += 4u;
        S.cmds++;
        g_hist[op]++;
        g_r[op] = arg;
        switch (op) {
        case 0x00: break;
        case 0x01: g_vaddr = rel; break;
        case 0x02: g_iaddr = rel; break;
        case 0x04: prim_run(s->pc - 4u, arg); break;
        case 0x08: s->pc = rel & ~3u; break;
        case 0x09: break; /* BJUMP requires GE bounding-box state; report as unsupported. */
        case 0x0A:
            if (s->sp >= 32) { S.lists_bad++; s->finished = 1; return -1; }
            s->ret[s->sp] = s->pc;
            s->saved_offset[s->sp] = g_offset;
            s->sp++;
            s->pc = rel & ~3u;
            break;
        case 0x0B:
            if (s->sp) {
                s->sp--;
                s->pc = s->ret[s->sp];
                g_offset = s->saved_offset[s->sp];
            }
            break;
        case 0x0C: s->finished = 1; return 1; /* END terminates the list. */
        case 0x0F: break; /* FINISH raises an event; commands may follow until END. */
        case 0x0E: break; /* SIGNAL next END is a real command; do NOT skip it. */
        case 0x10: g_base = arg; break;
        case 0x12:
            g_vtype = arg;
            g_n_vtype++;
            if (arg) g_n_vtype_nz++;
            break;
        case 0x13: g_offset = arg << 8; break;
        case 0x14: g_offset = s->pc - 4u; break;
        case 0x3A: g_wi = arg & 15u; break;
        case 0x3B: if (g_wi < 12u) g_world[g_wi++] = f24(arg); break;
        case 0x3C: g_vi = arg & 15u; break;
        case 0x3D: if (g_vi < 12u) g_view[g_vi++] = f24(arg); break;
        case 0x3E: g_pi = arg & 15u; break;
        case 0x3F: if (g_pi < 16u) g_proj[g_pi++] = f24(arg); break;
        default: S.ignored++; break;
        }
    }
    S.lists_bad++;
    s->finished = 1;
    return -1;
}
uint32_t ge_draw_list(uint32_t list, uint32_t stall) {
    ge_draw_list_state st;
    const uint32_t n0 = S.prims;
    ge_draw_start(&st, list, stall);
    ge_draw_resume(&st);
    return S.prims - n0;
}

void ge_draw_print_hist(void) {
    psp_printf("[hist] cmds=%u prims=%u lists=%u\n", S.cmds, S.prims, S.lists);
    for (int i = 0; i < 256; i++) {
        if (!g_hist[i]) continue;
        psp_printf("[hist] op=%02X n=%u\n", i, g_hist[i]);
    }
}

/* ---- public API ----------------------------------------------------------- */
void ge_draw_set_target(void *target) { g_target = target; }
void ge_draw_set_bottom(void *target) { g_bottom = target; }
void ge_draw_set_clear(uint32_t rgba) { g_clear = rgba; }
void ge_draw_set_magenta(int on) { g_magenta = on ? 1 : 0; }
int  ge_draw_magenta(void) { return g_magenta; }

void ge_draw_init(void) {
    memset(&S, 0, sizeof S);
    g_census_n = 0;
    g_vn = 0;
    g_frame_open = 0;
    g_ready = 0;
    g_vbase = 0;
    g_tex_bytes = g_tex_hits = g_tex_uploads = g_tex_fail = g_draw_drop = 0;
    g_preview_ready = 0; g_preview_frames = 0; g_guest_vertices_this_frame = 0;
    g_force_preview = 0;
    memset(g_texcache, 0, sizeof g_texcache);
    g_n_vtype = g_n_vtype_nz = 0;
    memset(g_hist, 0, sizeof g_hist);
    g_world[0] = g_world[4] = g_world[8] = 1.0f;
    g_view[0] = g_view[4] = g_view[8] = 1.0f;
    g_proj[0] = g_proj[5] = g_proj[10] = g_proj[15] = 1.0f;
    g_vtype = 0;
    g_vaddr = g_iaddr = 0;
    g_base = g_offset = 0;

    g_vb = (Vtx *)linearAlloc(VB_VERTS * sizeof(Vtx));
    if (!g_vb) return;

    g_dvlb = DVLB_ParseFile((u32 *)(void *)ge_vshader_shbin, ge_vshader_shbin_size);
    if (!g_dvlb) { psp_printf("[GE INIT ERROR] DVLB_ParseFile failed\n"); return; }
    shaderProgramInit(&g_prog);
    const int vsh = shaderProgramSetVsh(&g_prog, &g_dvlb->DVLE[0]);
    psp_printf("[ge_draw] dvlb=%p setVsh=0x%08X vb=%p\n", (void *)g_dvlb, (unsigned)vsh, (void *)g_vb);
    if (vsh != 0) return;
    C3D_BindProgram(&g_prog);
    g_uproj = shaderInstanceGetUniformLocation(g_prog.vertexShader, "projection");
    psp_printf("[ge_draw] uLoc projection=%d\n", g_uproj);
    if (g_uproj < 0) return;

    C3D_AttrInfo *ai = C3D_GetAttrInfo();
    AttrInfo_Init(ai);
    AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 2);
    AttrInfo_AddLoader(ai, 2, GPU_FLOAT, 4);

    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    C3D_TexEnvColor(env, 0xFF00FFFFu);   /* magenta opaco (fase C) */

    /* First-pass 2D sprite transparency. Full PSP GE blend-function mapping is
       still required for faithful effects and unsupported modes. */
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);

    /* The top LCD is rotated in GPU space; the standard 3DS projection is tilt.
     * Ortográficas en espacio PSP: la superior (400px) muestra 480x288 con
     * letterbox de 8 unidades; la inferior (320px) 480x360 con margen de 44. */
    Mtx_OrthoTilt(&g_mtx, 0.0f, PSP_W, PSP_H + 8.0f, -8.0f, 0.0f, 1.0f, true);
    Mtx_OrthoTilt(&g_mtx_bot, 0.0f, PSP_W, PSP_H + 44.0f, -44.0f, 0.0f, 1.0f, true);
    /* split: 362.6x272 PSP en la inferior (escala 0.882) y 453.3x272 en la
     * superior (modo 1), ambas centradas en el gear */
    Mtx_OrthoTilt(&g_mtx_split, g_vp_split.l, g_vp_split.r, PSP_H, 0.0f, 0.0f, 1.0f, true);
    Mtx_OrthoTilt(&g_mtx_split_top, g_vp_split_top.l, g_vp_split_top.r, PSP_H, 0.0f, 0.0f, 1.0f, true);
    g_cur_mtx = &g_mtx;
    g_cur_vp = &g_vp_top;
    {   /* layout elegido con SELECT: persiste entre sesiones */
        FILE *f = fopen("sdmc:/djmax/layout.bin", "rb");
        if (f) { const int c = fgetc(f); if (c >= 0) g_layout = c & 3; fclose(f); }
    }
    g_ready = 1;
    preview_init();
    romfsInit();                       /* assets del CIA; falla sin romfs (3dsx) y se ignora */
    {   /* logo de la pantalla inferior (menús): RGBA8 crudo 512x256; la SD manda sobre el romfs */
        FILE *f = fopen("sdmc:/djmax/tex/bottom_logo.rgba", "rb");
        if (!f) f = fopen("romfs:/tex/bottom_logo.rgba", "rb");
        if (f) {
            const size_t need = 512u * 256u * 4u;
            uint32_t *px = (uint32_t *)malloc(need);
            uint8_t  *pica = (uint8_t *)malloc(need);
            if (px && pica && fread(px, 1, need, f) == need &&
                C3D_TexInit(&g_logo, 512, 256, GPU_RGBA8)) {
                ge_tex_image img = { px, 512, 256 };
                ge_tex_to_pica_rgba8(&img, pica);
                C3D_TexUpload(&g_logo, pica);
                C3D_TexFlush(&g_logo);
                C3D_TexSetFilter(&g_logo, GPU_LINEAR, GPU_LINEAR);
                g_logo_ready = 1;
                psp_printf("[gfx] logo inferior cargado\n");
            }
            free(px); free(pica);
            fclose(f);
        }
    }
}

void ge_draw_frame(uint32_t fb, uint32_t sync) {
    (void)fb; (void)sync;
    /* Present even a frame without PRIM: otherwise the last selftest/old frame
       can remain on-screen and disguise a guest command-list failure. */
    if (!g_frame_open) frame_begin();
    if (g_force_preview || !g_guest_vertices_this_frame) preview_draw();
    if (g_split > 0) g_split--;
    {   /* TEMP [dual]: transiciones del split para afinar la detección */
        static int last_on = -1;
        const int on = g_split > 0;
        if (on != last_on) { last_on = on;
            psp_printf("[dual] split=%d f=%u\n", on, S.frames); }
    }
    g_gear_seen = 0;
    /* Logo en la pantalla sin juego: menús → inferior; layout 2 → superior;
     * layout 3 → inferior. En 0/1 ambas pantallas tienen juego. */
    int logo_scr = -1;
    if (g_bottom && g_logo_ready && g_frame_open) {
        if (g_split == 0)            logo_scr = 1;
        else if (g_layout == 2)      logo_scr = 0;
        else if (g_layout == 3)      logo_scr = 1;
    }
    if (logo_scr >= 0) {
        flush();
        g_route = logo_scr;
        g_cur_mtx = logo_scr ? &g_mtx_bot : &g_mtx;
        g_cur_vp  = logo_scr ? &g_vp_bot  : &g_vp_top;
        g_cur_depth = 3.0f;
        C3D_TexBind(0, &g_logo);
        C3D_TexEnv *env = C3D_GetTexEnv(0);
        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, 0, 0);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                       GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                       GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        const Vtx l0 = {  75,  72, 0.5f, 0, 1, 1, 1, 1, 1 };
        const Vtx l1 = { 405,  72, 0.5f, 1, 1, 1, 1, 1, 1 };
        const Vtx l2 = { 405, 237, 0.5f, 1, 0, 1, 1, 1, 1 };
        const Vtx l3 = {  75, 237, 0.5f, 0, 0, 1, 1, 1, 1 };
        tri(&l0, &l1, &l2); tri(&l0, &l2, &l3);
        flush();
        g_route = 0;
        g_cur_mtx = &g_mtx;
        g_cur_vp = &g_vp_top;
    }
    frame_end();
    g_guest_vertices_this_frame = 0;
    S.frames++;
    {   /* 60 Hz exactos: NONBLOCK quitó el pacing del SYNCDRAW. El deadline
         * mantiene vivos a los demás hilos (SAS) durante la espera. */
        extern uint64_t ps2_time_us(void);
        extern void ps2_audio_wait_until(uint64_t when_us);
        static uint64_t next_us;
        const uint64_t now = ps2_time_us();
        next_us += 16667u;
        if (!next_us || now > next_us + 100000u) next_us = now + 16667u;
        if (next_us > now) ps2_audio_wait_until(next_us);
        {   /* TEMP [fps]: medidor directo */
            static uint64_t t0; static uint32_t f0;
            if (!t0) { t0 = now; f0 = S.frames; }
            else if (S.frames - f0 >= 300u) {
                const uint32_t ms = (uint32_t)((now - t0) / 1000u);
                const uint32_t f10 = ms ? 3000000u / ms : 0u;      /* fps x10 */
                psp_printf("[fps] f=%u avg=%u.%u\n", S.frames, f10 / 10u, f10 % 10u);
                t0 = now; f0 = S.frames;
            }
        }
    }
}

void ge_draw_present(void) { frame_end(); }

int  ge_draw_split_active(void) { return g_split > 0; }
void ge_draw_cycle_layout(void) {
    g_layout = (g_layout + 1) & 3;
    psp_printf("[dual] layout=%d (SELECT)\n", g_layout);
    FILE *f = fopen("sdmc:/djmax/layout.bin", "wb");
    if (f) { fputc(g_layout, f); fclose(f); }
}
void ge_draw_set_right(void *target) { g_right = target; }

int ge_draw_ready(void) { return g_ready; }
uint32_t ge_draw_vtypes(void) { return g_n_vtype; }
uint32_t ge_draw_vtypes_nonzero(void) { return g_n_vtype_nz; }
void ge_draw_fini(void) {
    if (g_frame_open) frame_end();
    for (int i = 0; i < GE_TEXTURE_SLOTS; i++) texture_release(&g_texcache[i]);
    if (g_preview_ready) { C3D_TexDelete(&g_preview_tex); g_preview_ready = 0; }
    if (g_ready) shaderProgramFree(&g_prog);
    if (g_dvlb) { DVLB_Free(g_dvlb); g_dvlb = NULL; }
    if (g_vb) { linearFree(g_vb); g_vb = NULL; }
    g_ready = 0;
}
uint32_t ge_draw_prims(void) { return S.prims; }
uint32_t ge_draw_submitted(void) { return S.submitted; }
uint32_t ge_draw_rejected_no_pos(void) { return S.no_pos; }
uint32_t ge_draw_rejected_bad_addr(void) { return S.bad_addr; }
uint32_t ge_draw_rejected_other(void) { return S.other; }
uint32_t ge_draw_lists(void) { return S.lists; }
uint32_t ge_draw_cmds(void) { return S.cmds; }
uint32_t ge_draw_frames(void) { return S.frames; }
uint32_t ge_draw_textured(void) { return S.textured; }
uint32_t ge_draw_tex_bind(void) { return g_tex_uploads + g_tex_hits; }
uint32_t ge_draw_tex_fail(void) { return g_tex_fail; }
uint32_t ge_draw_tex_miss(void) { return g_tex_fail; }
int      ge_draw_census_count(void) { return g_census_n; }
const ge_census_call *ge_draw_census(int i) { return (i >= 0 && i < g_census_n) ? &g_census[i] : NULL; }
void     ge_draw_census_reset(void) { g_census_n = 0; }

int ge_draw_census_json(char *buf, size_t cap, uint32_t frame) {
    int n = snprintf(buf, cap,
        "{\n"
        "  \"frame\": %u,\n"
        "  \"ge_lists\": %u,\n"
        "  \"ge_cmds_decoded\": %u,\n"
        "  \"prim_encountered\": %u,\n"
        "  \"prim_rejected_no_position\": %u,\n"
        "  \"prim_rejected_bad_address\": %u,\n"
        "  \"prim_rejected_other\": %u,\n"
        "  \"prim_submitted_pica\": %u,\n"
        "  \"prims_with_texture\": %u,\n"
        "  \"cmds_ignored\": %u,\n"
        "  \"magenta_mode\": %d,\n"
        "  \"guest_draws\": [",
        frame, S.lists, S.cmds, S.prims, S.no_pos, S.bad_addr, S.other,
        S.submitted, S.textured, S.ignored, g_magenta);
    for (int i = 0; i < g_census_n; i++) {
        const ge_census_call *c = &g_census[i];
        n += snprintf(buf + (n < (int)cap ? n : (int)cap), n < (int)cap ? cap - (size_t)n : 0,
            "%s\n    { \"list_pc\": \"0x%08X\", \"prim_pc\": \"0x%08X\", \"prim_type\": %u, "
            "\"vertex_count\": %u, \"vtype\": \"0x%08X\", \"position_type\": %u, "
            "\"uv_type\": %u, \"color_type\": %u, \"index_type\": %u, \"through_2d\": %s, "
            "\"vertex_addr\": \"0x%08X\", \"vertex_stride\": %u, \"bbox_screen\": [%d,%d,%d,%d], "
            "\"texture_enabled\": %s, \"texture_addr\": \"0x%08X\", \"texture_psm\": %u, "
            "\"texture_wh\": [%u,%u], \"texture_stride\": %u, \"status\": \"%s\" }",
            (i ? 1 : 0) ? "," : "", c->list_pc, c->prim_pc, c->type, c->count, c->vtype,
            c->pos_type, c->uv_type, c->color_type, c->index_type,
            c->through_2d ? "true" : "false", c->vaddr, c->stride,
            c->bbox[0], c->bbox[1], c->bbox[2], c->bbox[3],
            c->tex_on ? "true" : "false", c->tex_addr, c->tex_psm, c->tex_w, c->tex_h,
            c->tex_stride, c->status);
    }
    n += snprintf(buf + (n < (int)cap ? n : (int)cap), n < (int)cap ? cap - (size_t)n : 0, "\n  ]\n}\n");
    return n;
}












