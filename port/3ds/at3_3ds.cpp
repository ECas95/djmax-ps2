/* at3_3ds.cpp -- ATRAC3 / ATRAC3+ para sceAtrac en el 3DS.
 *
 * Decoders de FFmpeg extraídos por PPSSPP (third_party/at3_standalone_src,
 * LGPL 2.1+). Adaptado de port/ps2/ps2_at3.cpp: aquí la arena es un buffer
 * estático (tablas VLC de AT3+ ~617 KB + contextos + scratch), no hay copia
 * muerta del módulo que reutilizar.
 */
#include "at3_decoders.h"

#include <string.h>

extern "C" {
void psp_printf(const char *fmt, ...);
}

/* ---- arena ---------------------------------------------------------------- */
static uint8_t  g_arena[2u * 1024u * 1024u] __attribute__((aligned(64)));
static uint32_t g_arena_used;

void *AllocateAlignedMemory(size_t size, size_t alignment) {
    if (alignment < 16) alignment = 16;
    const uint32_t at = (g_arena_used + (uint32_t)alignment - 1u) & ~((uint32_t)alignment - 1u);
    if (at + size > sizeof g_arena) {
        psp_printf("[at3] arena llena: %u + %u\n", at, (uint32_t)size);
        return nullptr;
    }
    g_arena_used = at + (uint32_t)size;
    return g_arena + at;
}

/* Arena de bump: los frees se descartan. Solo asigna la caché de decoders,
 * un número acotado de veces. */
void FreeAlignedMemory(void *) {}

/* ---- caché de decoders ------------------------------------------------------ */
struct ps2_at3 {
    int used, busy, plus, channels, bpf;
    uint8_t extra[14];
    ATRAC3Context *a3;
    ATRAC3PContext *a3p;
};
#define AT3_DECODERS 4
static ps2_at3 g_dec[AT3_DECODERS];

static inline int16_t f2s(float x) {
    const int v = (int)(x * 32768.0f);
    return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
}

extern "C" ps2_at3 *ps2_at3_get(int plus, int channels, int bpf, const uint8_t *extra, int extra_size) {
    uint8_t ex[14] = {0};
    if (extra && extra_size > 0) memcpy(ex, extra, extra_size < 14 ? extra_size : 14);
    for (int i = 0; i < AT3_DECODERS; i++) {
        ps2_at3 *d = &g_dec[i];
        if (d->used && !d->busy && d->plus == plus && d->channels == channels && d->bpf == bpf &&
            !memcmp(d->extra, ex, sizeof ex)) {
            if (plus) atrac3p_flush_buffers(d->a3p); else atrac3_flush_buffers(d->a3);
            d->busy = 1;
            return d;
        }
    }
    for (int i = 0; i < AT3_DECODERS; i++) {
        ps2_at3 *d = &g_dec[i];
        if (d->used) continue;
        int ba = bpf;
        if (plus) d->a3p = atrac3p_alloc(channels, &ba);
        else d->a3 = atrac3_alloc(channels, &ba, extra, extra_size);
        if (!d->a3 && !d->a3p) {
            psp_printf("[at3] alloc fallo plus=%d ch=%d bpf=%d extra=%d\n", plus, channels, bpf, extra_size);
            return nullptr;
        }
        d->used = d->busy = 1;
        d->plus = plus; d->channels = channels; d->bpf = bpf;
        memcpy(d->extra, ex, sizeof ex);
        psp_printf("[at3] decoder %d: plus=%d ch=%d bpf=%d, arena %u KB\n", i, plus, channels, bpf,
                   g_arena_used / 1024u);
        return d;
    }
    psp_printf("[at3] sin decoder libre\n");
    return nullptr;
}

extern "C" void ps2_at3_release(ps2_at3 *d) { if (d) d->busy = 0; }

/* Olvida los frames previos (salto en el stream: loop, reset). */
extern "C" void ps2_at3_flush(ps2_at3 *d) {
    if (!d) return;
    if (d->plus) atrac3p_flush_buffers(d->a3p); else atrac3_flush_buffers(d->a3);
}

/* Un frame (bpf bytes) -> s16 intercalado en out (hasta 2048 frames x canales).
 * Devuelve sample-frames, o <0 si el decode falla. */
extern "C" int ps2_at3_decode(ps2_at3 *d, const uint8_t *frame, int16_t *out) {
    static float *l, *r;
    if (!l) { l = (float *)AllocateAlignedMemory(2048 * 4, 64); r = (float *)AllocateAlignedMemory(2048 * 4, 64); }
    if (!l || !r) return -1;
    float *planes[2] = { l, r };
    int n = 0;
    const int e = d->plus ? atrac3p_decode_frame(d->a3p, planes, &n, frame, d->bpf)
                          : atrac3_decode_frame(d->a3, planes, &n, frame, d->bpf);
    if (e < 0) return e;
    if (d->channels == 2)
        for (int i = 0; i < n; i++) { out[2 * i] = f2s(l[i]); out[2 * i + 1] = f2s(r[i]); }
    else
        for (int i = 0; i < n; i++) out[i] = f2s(l[i]);
    return n;
}
