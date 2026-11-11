/* video_3ds.c -- 3DS output: libctru console on the bottom screen, the game's
 * 480x272 framebuffer scaled to the 400x240 top screen, and a log on sdmc.
 *
 * libctru returns the TOP framebuffer as 240x400 (the LCD is mounted rotated),
 * not 400x240: writing it as a landscape buffer silently does nothing. Every
 * pixel is therefore written rotated 90 degrees. The framebuffer is 3 bytes per
 * pixel (BGR8), confirmed by port/3ds/hello.
 */
#include "video_3ds.h"
#include "runtime_3ds.h"

#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>

#define TOP_W 400
#define TOP_H 240
#define FB_W  240
#define FB_H  400
#define PSP_W 480
#define PSP_H 272
#define PSP_STRIDE 512
/* Synthetic pattern to prove the presentation path before the real renderer.
 * 1 = colour bars + gradient, 0 = normal output. Kept behind a flag. */
#define VIDEO_TEST_PATTERN 0
#define PICA_DISPLAY_FLAGS (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | \
    GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | \
    GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))
static C3D_RenderTarget *g_pica_target;
static C3D_RenderTarget *g_pica_bottom;
static int g_pica_ready;
void *video_pica_target(void) { return g_pica_target; }
void *video_pica_bottom(void) { return g_pica_bottom; }
int video_pica_ready(void) { return g_pica_ready; }

static int      g_ready;
static int      g_log_ok;
static FILE    *g_log;
static uint32_t g_frames;

void video_init(void) {
    gfxInitDefault();
    /* Con el flag, la inferior es la consola de debug; sin él es un segundo
     * target GPU (gear en gameplay, logo en menús). */
    FILE *conflag = fopen("sdmc:/djmx_console.flag", "rb");
    const int want_console = conflag != NULL;
    if (conflag) fclose(conflag);
    if (want_console) {
        consoleInit(GFX_BOTTOM, NULL);
        gfxSetDoubleBuffering(GFX_BOTTOM, false);
    }
    g_pica_ready = C3D_Init(C3D_DEFAULT_CMDBUF_SIZE) ? 1 : 0;
    if (g_pica_ready) {
        g_pica_target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
        if (g_pica_target) {
            C3D_RenderTargetSetOutput(g_pica_target, GFX_TOP, GFX_LEFT, PICA_DISPLAY_FLAGS);
        } else {
            C3D_Fini();
            g_pica_ready = 0;
        }
        if (g_pica_ready && !want_console) {
            g_pica_bottom = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
            if (g_pica_bottom)
                C3D_RenderTargetSetOutput(g_pica_bottom, GFX_BOTTOM, GFX_LEFT, PICA_DISPLAY_FLAGS);
        }
    }

    u16 w = 0, h = 0;
    u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &w, &h);
    psp_printf("[gfx] top fb=%p %ux%u\n", (void *)fb, w, h);
    if (fb) memset(fb, 0, (size_t)w * h * 3u);

    fsInit();
    g_log = fopen("sdmc:/djmax3ds.log", "w");
    g_log_ok = g_log != NULL;
    /* En SD real, un fflush por línea cuesta milisegundos: buffer grande y
     * volcado periódico (video_log_flush) o al salir. */
    if (g_log) { static char lbuf[32768]; setvbuf(g_log, lbuf, _IOFBF, sizeof lbuf); }

    g_ready = 1;
    psp_printf("[pica] initialized=%d target=%p\n", g_pica_ready, (void *)g_pica_target);
    /* Only the fallback CPU backend writes the raw top framebuffer. */
    if (!g_pica_ready) { gfxFlushBuffers(); gfxSwapBuffers(); }
}

void video_shutdown(void) {
    if (g_log) { fflush(g_log); fclose(g_log); g_log = NULL; g_log_ok = 0; }
    fsExit();
    if (g_pica_bottom) { C3D_RenderTargetDelete(g_pica_bottom); g_pica_bottom = NULL; }
    if (g_pica_target) { C3D_RenderTargetDelete(g_pica_target); g_pica_target = NULL; }
    if (g_pica_ready) { C3D_Fini(); g_pica_ready = 0; }
    gfxExit();
}

uint32_t video_frames(void) { return g_frames; }
int video_log_ok(void) { return g_log_ok; }

void psp_console_write(const char *s) {
    if (!s || !*s) return;
    printf("%s", s);
    if (g_log) fputs(s, g_log);
}

void video_log_flush(void) {
    if (g_log) fflush(g_log);
}

/* Expand one RGB565 pixel into the 3DS BGR8 triplet. */
static inline void put_px(u8 *dst, uint16_t w, int x, int y, uint16_t p) {
    const uint32_t r = (p >> 11) & 31u, g = (p >> 5) & 63u, b = p & 31u;
    /* The top LCD is rotated: fb[x * 240 + (239 - y)]. */
    u8 *px = dst + ((size_t)x * w + (size_t)(TOP_H - 1 - y)) * 3u;
    px[0] = (u8)((b << 3) | (b >> 2));
    px[1] = (u8)((g << 2) | (g >> 4));
    px[2] = (u8)((r << 3) | (r >> 2));
}

/* 24-bit BMP of the raw top framebuffer (240x400, BGR8). Used as evidence for
 * the synthetic pattern: sdmc:/pattern.bmp. */
static void dump_bmp(const char *path, const u8 *fb, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    const uint32_t row = ((uint32_t)w * 3u + 3u) & ~3u;
    const uint32_t data = row * (uint32_t)h;
    u8 hdr[54]; memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    const uint32_t size = 54u + data;
    memcpy(hdr + 2, &size, 4);
    const uint32_t off = 54; memcpy(hdr + 10, &off, 4);
    const uint32_t hs = 40;  memcpy(hdr + 14, &hs, 4);
    memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4);
    const uint16_t one = 1; memcpy(hdr + 26, &one, 2);
    const uint16_t bpp = 24; memcpy(hdr + 28, &bpp, 2);
    memcpy(hdr + 34, &data, 4);
    fwrite(hdr, 1, 54, f);
    u8 pad[3] = {0,0,0};
    for (int y = h - 1; y >= 0; y--) {
        fwrite(fb + (size_t)y * w * 3u, 1, (size_t)w * 3u, f);
        fwrite(pad, 1, row - (uint32_t)w * 3u, f);
    }
    fclose(f);
}
void video_present(uint32_t guest_fb) {
    if (!g_ready || !guest_fb) return;
    g_frames++;
    /* Citro3D's display transfer already presents on GFX_TOP. Writing the raw
     * framebuffer afterward overwrites it and produces a misleading black screen. */
    if (g_pica_ready) return;

    uint16_t w = 0, h = 0;
    u8 *dst = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &w, &h);
    if (!dst || w != FB_W || h != FB_H) {
        static int once;
        if (!once) { once = 1; psp_printf("[gfx] present skip dst=%p %ux%u\n", (void *)dst, w, h); }
        return;
    }

    /* Is the guest frame buffer actually written? No GE rasteriser yet: if it
     * is all zero, show the 2 MB guest VRAM as a debug atlas instead. */
    if (VIDEO_TEST_PATTERN) {
        for (int y = 0; y < TOP_H; y++) {
            const uint32_t sy = (uint32_t)y;
            for (int x = 0; x < TOP_W; x++) {
                const uint32_t bar = (uint32_t)x * 8u / TOP_W;
                uint32_t r, g, b;
                switch (bar) {
                case 0: r=255; g=255; b=255; break;
                case 1: r=255; g=255; b=0;   break;
                case 2: r=0;   g=255; b=255; break;
                case 3: r=0;   g=255; b=0;   break;
                case 4: r=255; g=0;   b=255; break;
                case 5: r=255; g=0;   b=0;   break;
                case 6: r=0;   g=0;   b=255; break;
                default:r=0;   g=0;   b=0;   break;
                }
                r = (r * (240u - sy)) / 240u;
                g = (g * (240u - sy)) / 240u;
                b = (b * (240u - sy)) / 240u;
                u8 *px = dst + ((size_t)x * w + (size_t)(TOP_H - 1 - y)) * 3u;
                px[0] = (u8)b; px[1] = (u8)g; px[2] = (u8)r;
            }
        }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
        { static int dumped; if (!dumped) { dumped = 1; dump_bmp("sdmc:/pattern.bmp", dst, w, h); } }
        return;
    }

    static int s_black = -1;
    if (s_black < 0) {
        uint32_t nz = 0;
        for (uint32_t i = 0; i < PSP_W * PSP_H; i += 7u)
            if (psp_read16(guest_fb + (i / PSP_W) * PSP_STRIDE + (i % PSP_W) * 2u)) nz++;
        s_black = (nz == 0);
        uint32_t vnz = 0, colors = 0, seen = 0;
        const uint8_t *vr = psp_vram_ptr();
        for (uint32_t i = 0; i < PSP_VRAM_SIZE; i += 2u) {
            const uint16_t p = (uint16_t)(vr[i] | (vr[i + 1] << 8));
            if (p) vnz++;
            if ((i % 4096u) == 0 && colors < 64u) {
                uint32_t b;
                for (b = 0; b < seen; b++) if (((const uint16_t *)vr)[b * 2048u] == p) break;
                if (b == seen) { seen++; colors++; }
            }
        }
        psp_printf("[vram] fb=0x%08X black=%d vram_nonzero=%u sample_colors=%u\n",
                   guest_fb, s_black, vnz, colors);
    }

    if (s_black) {
        const uint8_t *vr = psp_vram_ptr();
        for (int y = 0; y < TOP_H; y++) {
            const uint32_t sy = (uint32_t)y * 1024u / TOP_H;
            for (int x = 0; x < TOP_W; x++) {
                const uint32_t sx = (uint32_t)x * 1024u / TOP_W;
                const uint32_t o = (sy * 1024u + sx) * 2u;
                put_px(dst, w, x, y, (uint16_t)(vr[o] | (vr[o + 1] << 8)));
            }
        }
    } else {
        for (int y = 0; y < TOP_H; y++) {
            const uint32_t sy = (uint32_t)y * PSP_H / TOP_H;
            for (int x = 0; x < TOP_W; x++) {
                const uint32_t sx = (uint32_t)x * PSP_W / TOP_W;
                put_px(dst, w, x, y,
                       psp_read16(guest_fb + sy * PSP_STRIDE + sx * 2u));
            }
        }
    }
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
}
