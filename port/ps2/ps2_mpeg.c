/* ps2_mpeg.c -- sceMpeg for the game's PSMF movies (opening, ending, ons, soundstorm).
 *
 * The movies are H.264, which the EE cannot decode in real time. tools-src/pmf_convert.py
 * turned each <name>.pmf into PS2_DATA/movie/<name>.m2v (MPEG-2, decoded by the IPU with
 * ps2sdk's libmpeg) and <name>.at3p (the movie's own ATRAC3+ frames, ps2_at3.cpp).
 *
 * The game's player keeps all its logic: it feeds the ring buffer through its callback
 * (called here, so it reads the .pmf to its end as on a PSP), takes AUs, decodes them
 * into RAM and copies them to the screen with GE transfers (ps2_ge.c draws those). Which
 * movie is playing is the last .pmf opened. The ring's packets count as consumed in
 * proportion to the frames handed out, so the game reads the file at the pace of the
 * movie; at the end GetAvcAu/GetAtracAu say "no data" as a PSP does.
 *
 * Memory: the game's ring is 7.8 MB of its own heap, and the .pmf bytes it holds are
 * never used (the movie comes from the converted files). The callback always writes the
 * first 64 KB; the rest serves libmpeg (its memalign/free are renamed to mpeg_memalign/
 * mpeg_free in a copy of the library, build.sh) and our buffers.
 *
 * Struct layouts (SceMpegRingbuffer, SceMpegAu with hi/lo-swapped timestamps) as PPSSPP
 * documents them.
 */
#include "runtime_min.h"
#include "ps2_hle.h"
#include "ps2_audio.h"

#include <dma.h>
#include <fcntl.h>
#include <kernel.h>
#include <libmpeg.h>
#include <string.h>
#include <unistd.h>

#define MPEG_NO_DATA   0x80618001u
#define PTS_START      90000ull
#define VIDEO_PTS_STEP 3003ull          /* 90 kHz / 29.97 */
#define AUDIO_PTS_STEP 4180ull          /* 90 kHz * 2048 / 44100 */
#define RING_CB_PACKETS 32u             /* the callback's window at the start of the ring */
#define CHUNK          (32u * 1024u)    /* .m2v bytes per DMA to the IPU */

typedef struct ps2_at3 ps2_at3;
ps2_at3 *ps2_at3_get(int plus, int channels, int bpf, const uint8_t *extra, int extra_size);
void ps2_at3_release(ps2_at3 *d);
int ps2_at3_decode(ps2_at3 *d, const uint8_t *frame, int16_t *out);

typedef struct { const char *name; uint32_t vframes, aframes; } movie_info;
static const movie_info g_movies[] = {      /* from pmf_convert.py */
    { "opening",    2629, 1891 },
    { "ending",     4109, 2955 },
    { "ons",        5037, 3622 },
    { "soundstorm", 3155, 2269 },
};

static char g_movie[16];                    /* basename of the last .pmf opened */

void ps2_mpeg_note_open(const char *path) {
    const size_t n = strlen(path);
    if (n < 5 || strcmp(path + n - 4, ".pmf") != 0) return;
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    size_t k = (size_t)(path + n - 4 - b);
    if (k >= sizeof g_movie) k = sizeof g_movie - 1;
    memcpy(g_movie, b, k);
    g_movie[k] = 0;
}

static struct {
    uint32_t ring;                          /* guest SceMpegRingbuffer */
    uint32_t packets, data, cb, cbarg;
    uint32_t written;                       /* packets the game put since Create */
    uint32_t stream_packets;                /* the .pmf's stream size / 2048 */
    uint32_t vframe, aframe, vframes, aframes;
    int active, vdone;
} M;

static uint32_t g_logged;
#define MLOG(...) do { if (g_logged < 60u) { g_logged++; psp_printf(__VA_ARGS__); } } while (0)

/* ---- arena (the game's ring past the callback window) ----------------------- */
static uint8_t *g_ar;
static uint32_t g_ar_size, g_ar_used;

void *mpeg_memalign(size_t align, size_t size) {
    if (!g_ar) return NULL;
    if (align < 16) align = 16;
    /* Align the address, not the offset: the arena base is only 256-byte aligned. */
    const uintptr_t base = (uintptr_t)g_ar;
    const uintptr_t p = (base + g_ar_used + align - 1u) & ~(uintptr_t)(align - 1u);
    const uint32_t at = (uint32_t)(p - base);
    if (at + size > g_ar_size) { psp_printf("[mpeg] arena full (%u + %u)\n", at, (uint32_t)size); return NULL; }
    g_ar_used = at + (uint32_t)size;
    return (void *)p;
}
/* ponytail: bump arena, reset at every movie; libmpeg frees only in MPEG_Destroy. */

/* The link wraps memalign/free (build.sh): inside a libmpeg call (g_in_mpeg) they use the
 * arena, everywhere else newlib's. A free of an arena pointer is dropped. */
static int g_in_mpeg;
void *__real_memalign(size_t align, size_t size);
void __real_free(void *p);
void *__wrap_memalign(size_t align, size_t size) {
    return g_in_mpeg ? mpeg_memalign(align, size) : __real_memalign(align, size);
}
void __wrap_free(void *p) {
    if (g_ar && (uint8_t *)p >= g_ar && (uint8_t *)p < g_ar + g_ar_size) return;
    __real_free(p);
}

/* ---- decoders ---------------------------------------------------------------- */
static int g_m2v = -1, g_a3p = -1;
static uint8_t *g_chunk[2];
static int g_ci, g_mpeg_up;
static uint8_t *g_pic;                      /* libmpeg output: RGBA32 in 16x16 macroblocks */
static int g_pw, g_ph;
static s64 g_cur_pts;
static ps2_at3 *g_adec;
static uint32_t g_abpf;
static uint8_t g_aframe_buf[1024];

static int mpeg_data_cb(void *u) {
    (void)u;
    if (g_m2v < 0) return 0;
    uint8_t *b = g_chunk[g_ci];
    int n = (int)read(g_m2v, b, CHUNK);
    if (n <= 0) return 0;
    if (n & 15) { memset(b + n, 0, 16 - (n & 15)); n = (n + 15) & ~15; }
    SyncDCache(b, b + n);
    dma_channel_wait(DMA_CHANNEL_toIPU, -1);   /* -1: until done (0 is a timeout that gives up) */
    dma_channel_send_normal(DMA_CHANNEL_toIPU, b, n >> 4, 0, 0);
    g_ci ^= 1;
    return 1;
}

/* The picture buffer exists before the first MPEG_Picture (which needs a target before
 * the sequence header is even read): every movie is 480x272 (pmf_convert.py). */
#define PIC_W 480
#define PIC_H 272
static void *mpeg_init_cb(void *u, MPEGSequenceInfo *si) {
    (void)u;
    g_pw = si->m_Width;
    g_ph = si->m_Height;
    psp_printf("[mpeg] sequence %dx%d, %d ms per frame, arena %u KB used\n", g_pw, g_ph, si->m_MSPerFrame, g_ar_used / 1024u);
    if (g_pw != PIC_W || g_ph != PIC_H) { g_pic = NULL; return mpeg_memalign(64, (size_t)g_pw * (size_t)g_ph * 4u); }
    return g_pic;
}

static void movie_close(void) {
    if (g_mpeg_up) { g_in_mpeg = 1; MPEG_Destroy(); g_in_mpeg = 0; g_mpeg_up = 0; }
    if (g_m2v >= 0) { close(g_m2v); g_m2v = -1; }
    if (g_a3p >= 0) { close(g_a3p); g_a3p = -1; }
    if (g_adec) { ps2_at3_release(g_adec); g_adec = NULL; }
    g_pic = NULL;
}

static void movie_open(void) {
    static int dma_up;
    char path[64];
    movie_close();
    g_ar = M.data ? (uint8_t *)psp_mem_ptr(M.data + RING_CB_PACKETS * 2048u, (M.packets - RING_CB_PACKETS) * 2048u) : NULL;
    g_ar_size = g_ar ? (M.packets - RING_CB_PACKETS) * 2048u : 0;
    g_ar_used = 0;
    g_chunk[0] = (uint8_t *)mpeg_memalign(64, CHUNK);
    g_chunk[1] = (uint8_t *)mpeg_memalign(64, CHUNK);
    g_pic = (uint8_t *)mpeg_memalign(64, PIC_W * PIC_H * 4u);
    g_pw = PIC_W; g_ph = PIC_H;
    g_ci = 0;
    if (!g_chunk[0] || !g_chunk[1] || !g_pic) { psp_printf("[mpeg] no arena: the movie stays black\n"); return; }

    snprintf(path, sizeof path, "host:PS2_DATA/movie/%s.m2v", g_movie);
    g_m2v = open(path, O_RDONLY);
    snprintf(path, sizeof path, "host:PS2_DATA/movie/%s.at3p", g_movie);
    g_a3p = open(path, O_RDONLY);
    psp_printf("[mpeg] %s: video fd=%d audio fd=%d, arena %u KB\n", g_movie, g_m2v, g_a3p, g_ar_size / 1024u);

    if (g_a3p >= 0) {
        uint32_t hdr[4] = {0};
        if (read(g_a3p, hdr, 16) == 16 && hdr[0] == 0x50335441u && hdr[1] <= sizeof g_aframe_buf) {   /* "AT3P" */
            g_abpf = hdr[1];
            g_adec = ps2_at3_get(1, (int)hdr[2], (int)hdr[1], NULL, 0);
        }
        if (!g_adec) { close(g_a3p); g_a3p = -1; }
    }
    if (g_m2v >= 0) {
        if (!dma_up) { dma_channel_initialize(DMA_CHANNEL_toIPU, NULL, 0); dma_up = 1; }
        g_in_mpeg = 1;
        MPEG_Initialize(mpeg_data_cb, NULL, mpeg_init_cb, NULL, &g_cur_pts);
        g_in_mpeg = 0;
        g_mpeg_up = 1;
    }
}

/* One picture into the guest frame (stride in pixels), 8888 with opaque alpha. */
static int video_next(uint32_t gbuf, uint32_t stride) {
    if (!g_mpeg_up || M.vdone) return 0;
    s64 pts;
    g_in_mpeg = 1;
    const int got = g_pic && MPEG_Picture(g_pic, &pts);
    g_in_mpeg = 0;
    if (!got) { M.vdone = 1; return 0; }
    const uint32_t mbw = (uint32_t)g_pw / 16u, mbh = (uint32_t)g_ph / 16u;
    uint32_t *dst = (uint32_t *)psp_mem_ptr(gbuf, stride * (uint32_t)g_ph * 4u);
    if (!dst || stride < (uint32_t)g_pw) return 0;
    InvalidDCache(g_pic, g_pic + (size_t)g_pw * (size_t)g_ph * 4u);
    const uint32_t *mb = (const uint32_t *)g_pic;
    for (uint32_t my = 0; my < mbh; my++)
        for (uint32_t mx = 0; mx < mbw; mx++, mb += 256)
            for (uint32_t r = 0; r < 16u; r++) {
                const uint32_t *s = mb + r * 16u;
                uint32_t *d = dst + (my * 16u + r) * stride + mx * 16u;
                for (uint32_t c = 0; c < 16u; c++) d[c] = s[c] | 0xFF000000u;
            }
    return 1;
}

static int audio_next(int16_t *out) {
    if (!g_adec || g_a3p < 0) return 0;
    if ((uint32_t)read(g_a3p, g_aframe_buf, g_abpf) != g_abpf) return 0;
    return ps2_at3_decode(g_adec, g_aframe_buf, out) > 0;
}

/* ---- helpers ------------------------------------------------------------------ */
static uint32_t be32(uint32_t a) {
    const uint8_t *p = (const uint8_t *)psp_mem_ptr(a, 4);
    return p ? (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3] : 0;
}

/* Call a guest function from an HLE call; every guest register comes back as it was. */
static uint32_t guest_call3(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
    static psp_cpu_state saved;             /* 800 B: not on the 64 KB EE stack */
    saved = psp_cpu;
    psp_cpu.r[PSP_REG_A0] = a0;
    psp_cpu.r[PSP_REG_A1] = a1;
    psp_cpu.r[PSP_REG_A2] = a2;
    psp_cpu.r[PSP_REG_SP] -= 64u;
    psp_call_from_host(fn);
    const uint32_t v0 = psp_cpu.r[PSP_REG_V0];
    psp_cpu = saved;
    return v0;
}

static void au_write(uint32_t au, uint64_t pts, uint64_t dts) {
    if (!au || !psp_mem_ptr(au, 24)) return;
    psp_write32(au + 0, (uint32_t)(pts >> 32));
    psp_write32(au + 4, (uint32_t)pts);
    psp_write32(au + 8, (uint32_t)(dts >> 32));
    psp_write32(au + 12, (uint32_t)dts);
}

/* packets of the ring the game may fill now */
static uint32_t ring_free(void) {
    uint32_t consumed = 0;
    if (M.vframes) consumed = (uint32_t)((uint64_t)M.stream_packets * M.vframe / M.vframes);
    const uint32_t held = M.written > consumed ? M.written - consumed : 0;
    return held >= M.packets ? 0 : M.packets - held;
}

static void ring_sync(void) {               /* the guest-visible counters */
    if (!M.ring || !M.packets || !psp_mem_ptr(M.ring, 44)) return;
    psp_write32(M.ring + 4, M.written % M.packets);   /* packetsRead (approx.) */
    psp_write32(M.ring + 8, M.written % M.packets);   /* packetsWritten */
    psp_write32(M.ring + 12, ring_free());            /* packetsAvail */
}

/* ---- HLE ------------------------------------------------------------------- */
static void hle_MpegInit(void)   { psp_ret(0); }
static void hle_MpegFinish(void) { movie_close(); psp_ret(0); }

static void hle_MpegRingbufferQueryMemSize(void) { psp_ret(psp_arg(0) * (2048u + 104u)); }

/* (ring*, packets, data, size, callback, cbarg) */
static void hle_MpegRingbufferConstruct(void) {
    const uint32_t r = psp_arg(0);
    M.ring = r;
    M.packets = psp_arg(1);
    M.data = psp_arg(2);
    M.cb = psp_arg(4);
    M.cbarg = psp_arg(5);
    M.written = 0;
    if (r && psp_mem_ptr(r, 44)) {
        psp_write32(r + 0, M.packets);
        psp_write32(r + 4, 0);
        psp_write32(r + 8, 0);
        psp_write32(r + 12, M.packets);
        psp_write32(r + 16, 2048);
        psp_write32(r + 20, M.data);
        psp_write32(r + 24, M.cb);
        psp_write32(r + 28, M.cbarg);
        psp_write32(r + 32, M.data + M.packets * 2048u);
        psp_write32(r + 36, 0xFFFFFFFFu);
        psp_write32(r + 40, 0);
    }
    MLOG("[mpeg] ring 0x%08X: %u packets at 0x%08X, callback 0x%08X\n", r, M.packets, M.data, M.cb);
    psp_ret(0);
}
static void hle_MpegRingbufferDestruct(void) { psp_ret(0); }

static void hle_MpegQueryMemSize(void) { psp_ret(0x10000u); }

/* (mpeg*, data, size, ring*, frameWidth, mode, ddrtop) */
static void hle_MpegCreate(void) {
    M.vframe = M.aframe = M.vframes = M.aframes = 0;
    M.vdone = 0;
    M.active = 1;
    if (psp_arg(0)) psp_write32(psp_arg(0), psp_arg(1));
    if (M.ring && psp_mem_ptr(M.ring, 44)) psp_write32(M.ring + 40, psp_arg(1));
    psp_ret(0);
}
static void hle_MpegDelete(void) {
    M.active = 0;
    MLOG("[mpeg] Delete after %u video / %u audio frames\n", M.vframe, M.aframe);
    movie_close();
    psp_ret(0);
}

/* (mpeg, buffer = .pmf header, offset*) */
static void hle_MpegQueryStreamOffset(void) {
    const uint32_t off = be32(psp_arg(1) + 8);
    if (psp_arg(2)) psp_write32(psp_arg(2), off);
    psp_ret(0);
}

/* (buffer, size*). The player creates the decoder before it opens the .pmf: the movie
 * is known from here on (the header it passes was read from the file just opened). */
static void hle_MpegQueryStreamSize(void) {
    const uint32_t sz = be32(psp_arg(0) + 12);
    M.stream_packets = (sz + 2047u) / 2048u;
    if (psp_arg(1)) psp_write32(psp_arg(1), sz);
    const movie_info *mi = NULL;
    for (unsigned i = 0; i < sizeof g_movies / sizeof g_movies[0]; i++)
        if (!strcmp(g_movies[i].name, g_movie)) mi = &g_movies[i];
    M.vframe = M.aframe = 0;
    M.vdone = 0;
    M.vframes = mi ? mi->vframes : 0;
    M.aframes = mi ? mi->aframes : 0;
    psp_printf("[mpeg] playing \"%s\": %u video / %u audio frames\n", g_movie, M.vframes, M.aframes);
    if (mi) movie_open();
    psp_ret(0);
}

static void hle_MpegRegistStream(void) { psp_ret(0x00340000u | (psp_arg(1) & 0xFFu)); }
static void hle_MpegOk(void) { psp_ret(0); }
static void hle_MpegMallocAvcEsBuf(void) { psp_ret(0x00030000u); }

static void hle_MpegInitAu(void) {          /* (mpeg, esbuf, au*) */
    au_write(psp_arg(2), ~0ull, ~0ull);
    psp_ret(0);
}

static void hle_MpegQueryAtracEsSize(void) { /* (mpeg, esSize*, outSize*) */
    if (psp_arg(1)) psp_write32(psp_arg(1), 2112);
    if (psp_arg(2)) psp_write32(psp_arg(2), 8192);
    psp_ret(0);
}

static void hle_MpegRingbufferAvailableSize(void) { psp_ret(ring_free()); }

/* (ring, numPackets, available): the game's callback reads the .pmf into the ring's
 * first RING_CB_PACKETS packets (the rest of the ring is our arena). */
static void hle_MpegRingbufferPut(void) {
    uint32_t want = psp_arg(1);
    const uint32_t f = ring_free();
    if (want > f) want = f;
    uint32_t got = 0;
    while (want && M.cb) {
        const uint32_t n = want < RING_CB_PACKETS ? want : RING_CB_PACKETS;
        const int32_t r = (int32_t)guest_call3(M.cb, M.data, n, M.cbarg);
        if (r <= 0) break;
        M.written += (uint32_t)r;
        got += (uint32_t)r;
        want -= (uint32_t)r;
        if ((uint32_t)r < n) break;
    }
    ring_sync();
    psp_ret(got);
}

/* (mpeg, stream, au*, attr*) */
static void hle_MpegGetAvcAu(void) {
    if (!M.active || M.vdone || M.vframe >= M.vframes) { psp_ret(MPEG_NO_DATA); return; }
    const uint64_t pts = PTS_START + VIDEO_PTS_STEP * M.vframe;
    au_write(psp_arg(2), pts, pts);
    if (psp_arg(3)) psp_write32(psp_arg(3), 1);
    M.vframe++;
    ring_sync();
    psp_ret(0);
}

/* (mpeg, au*, frameWidth, buffer**, init*) */
static void hle_MpegAvcDecode(void) {
    const uint32_t bufp = psp_arg(3);
    const uint32_t buf = bufp ? psp_read32(bufp) : 0;
    const int ok = buf && video_next(buf, psp_arg(2));
    if (M.vframe <= 2u || M.vframe % 900u == 0u)
        MLOG("[mpeg] AvcDecode %u -> 0x%08X %s\n", M.vframe, buf, ok ? "ok" : "no picture");
    if (psp_arg(4)) psp_write32(psp_arg(4), 1);
    psp_ret(0);
}

static void hle_MpegAvcDecodeStop(void) {   /* (mpeg, frameWidth, buffer, status*) */
    if (psp_arg(3)) psp_write32(psp_arg(3), 0);
    psp_ret(0);
}

/* (mpeg, stream, au*, attr*) */
static void hle_MpegGetAtracAu(void) {
    if (!M.active || M.aframe >= M.aframes) { psp_ret(MPEG_NO_DATA); return; }
    const uint64_t pts = PTS_START + AUDIO_PTS_STEP * M.aframe;
    au_write(psp_arg(2), pts, pts);
    if (psp_arg(3)) psp_write32(psp_arg(3), 0);
    M.aframe++;
    psp_ret(0);
}

/* (mpeg, au*, buffer, init): one ATRAC3+ frame, 2048 stereo s16 samples */
static void hle_MpegAtracDecode(void) {
    int16_t *out = psp_arg(2) ? (int16_t *)psp_mem_ptr(psp_arg(2), 8192) : NULL;
    if (out && !audio_next(out)) memset(out, 0, 8192);
    psp_ret(0);
}

void ps2_mpeg_install(void) {
    psp_hle_register(0x682A619B, "sceMpeg", "sceMpegInit",                  hle_MpegInit);
    psp_hle_register(0x874624D6, "sceMpeg", "sceMpegFinish",                hle_MpegFinish);
    psp_hle_register(0xC132E22F, "sceMpeg", "sceMpegQueryMemSize",          hle_MpegQueryMemSize);
    psp_hle_register(0xD7A29F46, "sceMpeg", "sceMpegRingbufferQueryMemSize", hle_MpegRingbufferQueryMemSize);
    psp_hle_register(0x37295ED8, "sceMpeg", "sceMpegRingbufferConstruct",   hle_MpegRingbufferConstruct);
    psp_hle_register(0xB240A59E, "sceMpeg", "sceMpegRingbufferPut",         hle_MpegRingbufferPut);
    psp_hle_register(0xB5F6DC87, "sceMpeg", "sceMpegRingbufferAvailableSize", hle_MpegRingbufferAvailableSize);
    psp_hle_register(0x13407F13, "sceMpeg", "sceMpegRingbufferDestruct",    hle_MpegRingbufferDestruct);
    psp_hle_register(0xD8C5F121, "sceMpeg", "sceMpegCreate",                hle_MpegCreate);
    psp_hle_register(0x606A4649, "sceMpeg", "sceMpegDelete",                hle_MpegDelete);
    psp_hle_register(0x42560F23, "sceMpeg", "sceMpegRegistStream",          hle_MpegRegistStream);
    psp_hle_register(0x591A4AA2, "sceMpeg", "sceMpegUnRegistStream",        hle_MpegOk);
    psp_hle_register(0x21FF80E4, "sceMpeg", "sceMpegQueryStreamOffset",     hle_MpegQueryStreamOffset);
    psp_hle_register(0x611E9E11, "sceMpeg", "sceMpegQueryStreamSize",       hle_MpegQueryStreamSize);
    psp_hle_register(0x167AFD9E, "sceMpeg", "sceMpegInitAu",                hle_MpegInitAu);
    psp_hle_register(0xA780CF7E, "sceMpeg", "sceMpegMallocAvcEsBuf",        hle_MpegMallocAvcEsBuf);
    psp_hle_register(0xCEB870B1, "sceMpeg", "sceMpegFreeAvcEsBuf",          hle_MpegOk);
    psp_hle_register(0x0E3C2E9D, "sceMpeg", "sceMpegAvcDecode",             hle_MpegAvcDecode);
    psp_hle_register(0x740FCCD1, "sceMpeg", "sceMpegAvcDecodeStop",         hle_MpegAvcDecodeStop);
    psp_hle_register(0x4571CC64, "sceMpeg", "sceMpegAvcDecodeFlush",        hle_MpegOk);
    psp_hle_register(0x800C44DF, "sceMpeg", "sceMpegAtracDecode",           hle_MpegAtracDecode);
    psp_hle_register(0xFE246728, "sceMpeg", "sceMpegGetAvcAu",              hle_MpegGetAvcAu);
    psp_hle_register(0xE1CE83A7, "sceMpeg", "sceMpegGetAtracAu",            hle_MpegGetAtracAu);
    psp_hle_register(0xF8DCB679, "sceMpeg", "sceMpegQueryAtracEsSize",      hle_MpegQueryAtracEsSize);
}
