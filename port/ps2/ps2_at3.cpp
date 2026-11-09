/* ps2_at3.cpp -- ATRAC3 / ATRAC3+ decoding for sceAtrac, on the EE.
 *
 * The decoders are FFmpeg's, as extracted by PPSSPP (third_party/at3_standalone_src,
 * LGPL 2.1+, one memory patch: PATCHES.txt). This file gives them memory and a C API.
 *
 * Memory: an arena over the boot copy of the module (see ps2_at3_init). Decoders are
 * cached and reused (flushed) instead of freed: a song change allocates nothing.
 *
 * host:at3test.txt (one line: a host path to an .at3) runs ps2_at3_selftest at boot:
 * decodes the file, prints the EE cycles per frame and writes host:at3test.raw (s16).
 */
#include "at3_decoders.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

extern "C" {
#include "runtime_min.h"
#include "ps2_hle.h"
}

/* ---- arena ---------------------------------------------------------------- */
/* The ELF's copy of the module's first segment (923 KB, main.c/build.sh): dead once
 * main has copied it into g_module. Holds the 617 KB of ATRAC3+ VLC tables, the decoder
 * contexts and the PCM scratch. */
static uint8_t *g_arena;
static uint32_t g_arena_size, g_arena_used;

extern "C" void ps2_at3_init(void *mem, uint32_t size) {
    g_arena = (uint8_t *)mem;
    g_arena_size = size;
    psp_printf("[at3] arena %p (%u KB, the boot copy of the module)\n", mem, size / 1024u);
}

void *AllocateAlignedMemory(size_t size, size_t alignment) {
    if (!g_arena) return nullptr;
    if (alignment < 16) alignment = 16;
    const uint32_t at = (g_arena_used + (uint32_t)alignment - 1u) & ~((uint32_t)alignment - 1u);
    if (at + size > g_arena_size) {
        psp_printf("[at3] arena full: %u + %u\n", at, (uint32_t)size);
        return nullptr;
    }
    g_arena_used = at + (uint32_t)size;
    return g_arena + at;
}

/* ponytail: bump arena, frees are dropped. Only the decoder cache below allocates,
 * a bounded number of times. */
void FreeAlignedMemory(void *) {}

/* ---- decoder cache --------------------------------------------------------- */
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
            psp_printf("[at3] alloc failed plus=%d ch=%d bpf=%d extra=%d\n", plus, channels, bpf, extra_size);
            return nullptr;
        }
        d->used = d->busy = 1;
        d->plus = plus; d->channels = channels; d->bpf = bpf;
        memcpy(d->extra, ex, sizeof ex);
        psp_printf("[at3] decoder %d: plus=%d ch=%d bpf=%d, arena %u KB used\n", i, plus, channels, bpf,
                   g_arena_used / 1024u);
        return d;
    }
    psp_printf("[at3] no free decoder\n");
    return nullptr;
}

extern "C" void ps2_at3_release(ps2_at3 *d) { if (d) d->busy = 0; }

/* Forget the previous frames (a jump in the stream: loop, reset). */
extern "C" void ps2_at3_flush(ps2_at3 *d) {
    if (!d) return;
    if (d->plus) atrac3p_flush_buffers(d->a3p); else atrac3_flush_buffers(d->a3);
}

/* One frame (bpf bytes) -> interleaved s16 at out (up to 2048 frames x channels).
 * Returns the number of sample frames, or <0 on a decode error. */
extern "C" int ps2_at3_decode(ps2_at3 *d, const uint8_t *frame, int16_t *out) {
    static float *l, *r;                 /* in the arena: the ELF has no room */
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

/* ---- self-test --------------------------------------------------------------- */
static inline uint32_t cop0_count() { uint32_t c; __asm__ volatile("mfc0 %0, $9" : "=r"(c)); return c; }

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void selftest_one(const char *path, int idx) {
    int fd;
    uint8_t hdr[512];
    fd = open(path, O_RDONLY);
    if (fd < 0) { psp_printf("[at3test] cannot open %s\n", path); return; }
    read(fd, hdr, sizeof hdr);
    /* RIFF: fmt and data chunks */
    uint32_t off = 12, data = 0, ch = 0, bpf = 0, tag = 0, exn = 0;
    const uint8_t *ex = nullptr;
    while (off + 8 <= sizeof hdr) {
        const uint32_t id = rd32(hdr + off), sz = rd32(hdr + off + 4);
        if (id == 0x20746D66u) {               /* "fmt " */
            tag = hdr[off + 8] | hdr[off + 9] << 8;
            ch = hdr[off + 10] | hdr[off + 11] << 8;
            bpf = hdr[off + 20] | hdr[off + 21] << 8;
            exn = hdr[off + 24] | hdr[off + 25] << 8;
            ex = hdr + off + 26;
        } else if (id == 0x61746164u) {         /* "data" */
            data = off + 8;
            break;
        }
        off += 8 + sz;
    }
    const int plus = tag == 0xFFFEu;
    psp_printf("[at3test] %s tag=0x%X ch=%u bpf=%u extra=%u data=%u\n", path, tag, ch, bpf, exn, data);
    ps2_at3 *d = data ? ps2_at3_get(plus, (int)ch, (int)bpf, plus ? nullptr : ex, plus ? 0 : (int)exn) : nullptr;
    if (!d) { close(fd); return; }
    lseek(fd, (int)data, SEEK_SET);
    char outname[32] = "host:at3test0.raw";
    outname[12] = (char)('0' + idx);
    const int out = open(outname, O_WRONLY | O_CREAT | O_TRUNC);
    uint8_t *frame = (uint8_t *)AllocateAlignedMemory(2048, 64);
    int16_t *pcm = (int16_t *)AllocateAlignedMemory(2048 * 4, 64);
    if (!frame || !pcm) { close(fd); return; }
    uint64_t cyc = 0;
    uint32_t frames = 0, samples = 0, worst = 0, errs = 0;
    while (frames < 2000u && read(fd, frame, bpf) == (int)bpf) {
        const uint32_t t0 = cop0_count();
        const int k = ps2_at3_decode(d, frame, pcm);
        const uint32_t dt = cop0_count() - t0;
        cyc += dt;
        if (dt > worst) worst = dt;
        frames++;
        if (k < 0) { errs++; continue; }
        samples += (uint32_t)k;
        if (out >= 0) write(out, pcm, k * (int)ch * 2);
    }
    close(fd);
    if (out >= 0) close(out);
    ps2_at3_release(d);
    const uint32_t per = frames ? (uint32_t)(cyc / frames) : 0;
    /* EE share for real-time playback: cycles per frame / cycles per frame of audio */
    const uint32_t spf = frames ? samples / frames : 1;
    const uint32_t budget = (uint32_t)(294912000ull * spf / 44100ull);
    psp_printf("[at3test] frames=%u samples=%u errs=%u  %u cycles/frame (worst %u)  = %u.%u%% of the EE in real time\n",
               frames, samples, errs, per, worst, budget ? per * 100u / budget : 0u,
               budget ? (per * 1000u / budget) % 10u : 0u);
    psp_printf("[at3test] arena %u KB used\n", g_arena_used / 1024u);
}

/* host:at3test.txt: one host path per line; outputs host:at3test<N>.raw */
extern "C" void ps2_at3_selftest(void) {
    char text[512];
    const int fd = open("host:at3test.txt", O_RDONLY);
    if (fd < 0) return;
    const int n = (int)read(fd, text, sizeof text - 1);
    close(fd);
    if (n <= 0) return;
    text[n] = 0;
    int idx = 0;
    for (char *p = text; *p && idx < 10; ) {
        char *e = p;
        while (*e && *e != '\r' && *e != '\n') e++;
        const char c = *e;
        *e = 0;
        if (e > p) selftest_one(p, idx++);
        if (!c) break;
        p = e + 1;
    }
}
