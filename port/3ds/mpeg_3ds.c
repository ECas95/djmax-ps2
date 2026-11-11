/* mpeg_3ds.c -- sceMpeg for the game's PSMF movies, without a decoder.
 *
 * Same contract as port/ps2/ps2_mpeg.c (the one the playable PS2 build uses): the
 * game's player keeps all its logic, feeds the ring through its own callback (so it
 * reads the .pmf to the end), takes AUs and closes the movie by its normal path when
 * GetAvcAu/GetAtracAu report "no data". There is no H.264 decoder on the ARM11, so the
 * pictures stay black and every movie is cut to MOVIE_MAX_FRAMES.
 */
#include <string.h>

#include "runtime_3ds.h"
#include "hle_3ds.h"

#define MPEG_NO_DATA   0x80618001u
#define MPEG_EOS       0x80618005u
#define PTS_START      90000ull
#define VIDEO_PTS_STEP 3003ull
#define AUDIO_PTS_STEP 4180ull
#define RING_CB_PACKETS 32u

#ifndef MOVIE_MAX_FRAMES
#define MOVIE_MAX_FRAMES 30u
#endif

typedef struct { const char *name; uint32_t vframes, aframes; } movie_info;
static const movie_info g_movies[] = {
    { "opening",    2629, 1891 },
    { "ending",     4109, 2955 },
    { "ons",        5037, 3622 },
    { "soundstorm", 3155, 2269 },
};

static char g_movie[16];

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
    uint32_t ring;
    uint32_t packets, data, cb, cbarg;
    uint32_t written;
    uint32_t stream_packets;
    uint32_t vframe, aframe, vframes, aframes;
    int active;
} M;

static uint32_t be32(uint32_t a) {
    const uint8_t *p = (const uint8_t *)psp_mem_ptr(a, 4);
    return p ? (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3] : 0;
}

static uint32_t guest_call3(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
    static psp_cpu_state saved;
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

static uint32_t ring_free(void) {
    uint32_t consumed = 0;
    if (M.vframes) consumed = (uint32_t)((uint64_t)M.stream_packets * M.vframe / M.vframes);
    const uint32_t held = M.written > consumed ? M.written - consumed : 0;
    return held >= M.packets ? 0 : M.packets - held;
}

static void ring_sync(void) {
    if (!M.ring || !M.packets || !psp_mem_ptr(M.ring, 44)) return;
    psp_write32(M.ring + 4, M.written % M.packets);
    psp_write32(M.ring + 8, M.written % M.packets);
    psp_write32(M.ring + 12, ring_free());
}

static void hle_MpegOk(void) { psp_ret(0); }
static void hle_MpegRingbufferQueryMemSize(void) { psp_ret(psp_arg(0) * (2048u + 104u)); }
static void hle_MpegQueryMemSize(void) { psp_ret(0x10000u); }

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
    psp_printf("[mpeg] ring 0x%08X: %u packets at 0x%08X, callback 0x%08X\n", r, M.packets, M.data, M.cb);
    psp_ret(0);
}

static void hle_MpegCreate(void) {
    M.vframe = M.aframe = M.vframes = M.aframes = 0;
    M.active = 1;
    if (psp_arg(0) && psp_mem_ptr(psp_arg(0), 4)) psp_write32(psp_arg(0), psp_arg(1));
    if (M.ring && psp_mem_ptr(M.ring, 44)) psp_write32(M.ring + 40, psp_arg(1));
    psp_ret(0);
}

static void hle_MpegDelete(void) {
    M.active = 0;
    psp_printf("[mpeg] Delete after %u video / %u audio frames\n", M.vframe, M.aframe);
    psp_ret(0);
}

static void hle_MpegQueryStreamOffset(void) {
    const uint32_t off = be32(psp_arg(1) + 8);
    if (psp_arg(2) && psp_mem_ptr(psp_arg(2), 4)) psp_write32(psp_arg(2), off);
    psp_ret(0);
}

static void hle_MpegQueryStreamSize(void) {
    const uint32_t sz = be32(psp_arg(0) + 12);
    M.stream_packets = (sz + 2047u) / 2048u;
    if (psp_arg(1) && psp_mem_ptr(psp_arg(1), 4)) psp_write32(psp_arg(1), sz);
    const movie_info *mi = NULL;
    for (unsigned i = 0; i < sizeof g_movies / sizeof g_movies[0]; i++)
        if (!strcmp(g_movies[i].name, g_movie)) mi = &g_movies[i];
    M.vframe = M.aframe = 0;
    M.vframes = mi ? mi->vframes : MOVIE_MAX_FRAMES;
    M.aframes = mi ? mi->aframes : MOVIE_MAX_FRAMES;
    if (M.vframes > MOVIE_MAX_FRAMES) {
        M.aframes = (uint32_t)((uint64_t)M.aframes * MOVIE_MAX_FRAMES / M.vframes);
        M.vframes = MOVIE_MAX_FRAMES;
    }
    psp_printf("[mpeg] playing \"%s\" (%u B): %u video / %u audio frames\n",
               g_movie, sz, M.vframes, M.aframes);
    psp_ret(0);
}

static void hle_MpegRegistStream(void) { psp_ret(0x00340000u | (psp_arg(1) & 0xFFu)); }
static void hle_MpegMallocAvcEsBuf(void) { psp_ret(0x00030000u); }

static void hle_MpegInitAu(void) {
    au_write(psp_arg(2), ~0ull, ~0ull);
    psp_ret(0);
}

static void hle_MpegQueryAtracEsSize(void) {
    if (psp_arg(1) && psp_mem_ptr(psp_arg(1), 4)) psp_write32(psp_arg(1), 2112);
    if (psp_arg(2) && psp_mem_ptr(psp_arg(2), 4)) psp_write32(psp_arg(2), 8192);
    psp_ret(0);
}

static void hle_MpegRingbufferAvailableSize(void) { psp_ret(ring_free()); }

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

static void hle_MpegGetAvcAu(void) {
    /* The pump (0x8B230) retries 0x80618001 forever in its inner loop (0x8B434 ->
     * 0x8B3C4); at the end of the cut movie answer another error so it leaves by its
     * own clean abort path (0x8B500 -> 0x8C350 -> 0x8B514). */
    if (M.active && M.vframe >= M.vframes) {
        static uint32_t eos_logged;
        if (!eos_logged++) psp_printf("[mpeg] end of movie after %u frames -> EOS\n", M.vframe);
        psp_ret(MPEG_EOS);
        return;
    }
    if (!M.active) { psp_ret(MPEG_NO_DATA); return; }
    const uint64_t pts = PTS_START + VIDEO_PTS_STEP * M.vframe;
    au_write(psp_arg(2), pts, pts);
    if (psp_arg(3) && psp_mem_ptr(psp_arg(3), 4)) psp_write32(psp_arg(3), 1);
    M.vframe++;
    ring_sync();
    psp_ret(0);
}

static void hle_MpegAvcDecode(void) {
    if (psp_arg(4) && psp_mem_ptr(psp_arg(4), 4)) psp_write32(psp_arg(4), 1);
    psp_ret(0);
}

static void hle_MpegAvcDecodeStop(void) {
    if (psp_arg(3) && psp_mem_ptr(psp_arg(3), 4)) psp_write32(psp_arg(3), 0);
    psp_ret(0);
}

static void hle_MpegGetAtracAu(void) {
    if (!M.active || M.aframe >= M.aframes) { psp_ret(MPEG_NO_DATA); return; }
    const uint64_t pts = PTS_START + AUDIO_PTS_STEP * M.aframe;
    au_write(psp_arg(2), pts, pts);
    if (psp_arg(3) && psp_mem_ptr(psp_arg(3), 4)) psp_write32(psp_arg(3), 0);
    M.aframe++;
    psp_ret(0);
}

static void hle_MpegAtracDecode(void) {
    uint8_t *out = psp_arg(2) ? (uint8_t *)psp_mem_ptr(psp_arg(2), 8192) : NULL;
    if (out) memset(out, 0, 8192);
    psp_ret(0);
}

void ps2_mpeg_install(void) {
    psp_hle_register(0x682A619B, "sceMpeg", "sceMpegInit",                  hle_MpegOk);
    psp_hle_register(0x874624D6, "sceMpeg", "sceMpegFinish",                hle_MpegOk);
    psp_hle_register(0xC132E22F, "sceMpeg", "sceMpegQueryMemSize",          hle_MpegQueryMemSize);
    psp_hle_register(0xD7A29F46, "sceMpeg", "sceMpegRingbufferQueryMemSize", hle_MpegRingbufferQueryMemSize);
    psp_hle_register(0x37295ED8, "sceMpeg", "sceMpegRingbufferConstruct",   hle_MpegRingbufferConstruct);
    psp_hle_register(0xB240A59E, "sceMpeg", "sceMpegRingbufferPut",         hle_MpegRingbufferPut);
    psp_hle_register(0xB5F6DC87, "sceMpeg", "sceMpegRingbufferAvailableSize", hle_MpegRingbufferAvailableSize);
    psp_hle_register(0x13407F13, "sceMpeg", "sceMpegRingbufferDestruct",    hle_MpegOk);
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
