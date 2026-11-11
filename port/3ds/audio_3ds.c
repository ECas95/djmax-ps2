/* audio_3ds.c -- backend de audio: mezcla los canales sceAudio del PSP y los
 * entrega a ndsp (44100 Hz estéreo s16).
 *
 * Cada canal PSP tiene un FIFO de frames estéreo. El hilo de ndsp los suma en
 * trozos de CHUNK frames y los encola como wave buffers. El HLE (hle_3ds.c)
 * bloquea al guest cuando su FIFO va lleno: eso ancla el hilo de audio del
 * juego al tiempo real, igual que el hardware del PSP.
 *
 * En Citra/Azahar el DSP es HLE y el contenido de dspfirm.cdc se ignora, pero
 * libctru exige que el archivo exista: si falta se escribe uno vacío.
 */
#include <3ds.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "audio_3ds.h"

void psp_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

uint64_t ps2_time_us(void) {
    /* ARM11 system tick: 268.123480 MHz on both Old and New 3DS. */
    return (uint64_t)svcGetSystemTick() * 1000000ull / 268123480ull;
}

void ps2_audio_preempt(void) { }

#define FIFO_FRAMES 16384u              /* potencia de 2; ~0.37 s por canal */
#define CHUNK       1024u               /* frames por wave buffer (~23 ms) */
#define NBUF        3

static int16_t  g_fifo[PS2_AUDIO_CH][FIFO_FRAMES * 2];
static uint32_t g_rd[PS2_AUDIO_CH], g_wr[PS2_AUDIO_CH];

static ndspWaveBuf g_wb[NBUF];
static int16_t    *g_wmem;
static int         g_ok;

int ps2_audio_ok(void) { return g_ok; }

uint32_t ps2_audio_level(int ch) {
    if (ch < 0 || ch >= PS2_AUDIO_CH) return 0;
    return g_wr[ch] - __atomic_load_n(&g_rd[ch], __ATOMIC_ACQUIRE);
}

void ps2_audio_reset(int ch) {
    if (ch < 0 || ch >= PS2_AUDIO_CH) return;
    __atomic_store_n(&g_wr[ch], __atomic_load_n(&g_rd[ch], __ATOMIC_ACQUIRE),
                     __ATOMIC_RELEASE);
}

void ps2_audio_push(int ch, const int16_t *lr, uint32_t frames) {
    if (ch < 0 || ch >= PS2_AUDIO_CH || !g_ok) return;
    const uint32_t wr = g_wr[ch];
    uint32_t space = FIFO_FRAMES - (wr - __atomic_load_n(&g_rd[ch], __ATOMIC_ACQUIRE));
    if (frames > space) frames = space;          /* el HLE bloquea antes: no debería pasar */
    for (uint32_t i = 0; i < frames; i++) {
        const uint32_t s = (wr + i) & (FIFO_FRAMES - 1u);
        g_fifo[ch][s * 2u]      = lr[i * 2u];
        g_fifo[ch][s * 2u + 1u] = lr[i * 2u + 1u];
    }
    __atomic_store_n(&g_wr[ch], wr + frames, __ATOMIC_RELEASE);
}

/* Suma todos los FIFOs en un trozo de salida; canal sin datos aporta silencio. */
static void mix_pull(int16_t *dst, uint32_t frames) {
    memset(dst, 0, (size_t)frames * 2u * sizeof(int16_t));
    for (int c = 0; c < PS2_AUDIO_CH; c++) {
        const uint32_t rd    = g_rd[c];
        const uint32_t avail = __atomic_load_n(&g_wr[c], __ATOMIC_ACQUIRE) - rd;
        const uint32_t n     = avail < frames ? avail : frames;
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t s = (rd + i) & (FIFO_FRAMES - 1u);
            int l = dst[i * 2u]      + g_fifo[c][s * 2u];
            int r = dst[i * 2u + 1u] + g_fifo[c][s * 2u + 1u];
            if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
            if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
            dst[i * 2u]      = (int16_t)l;
            dst[i * 2u + 1u] = (int16_t)r;
        }
        __atomic_store_n(&g_rd[c], rd + n, __ATOMIC_RELEASE);
    }
}

static void ndsp_cb(void *ud) {
    (void)ud;
    if (!g_ok) return;
    for (int i = 0; i < NBUF; i++) {
        if (g_wb[i].status != NDSP_WBUF_DONE && g_wb[i].status != NDSP_WBUF_FREE)
            continue;
        int16_t *p = g_wmem + (size_t)i * CHUNK * 2u;
        mix_pull(p, CHUNK);
        DSP_FlushDataCache(p, CHUNK * 2u * sizeof(int16_t));
        g_wb[i].nsamples = CHUNK;
        ndspChnWaveBufAdd(0, &g_wb[i]);
    }
}

/* libctru lee el componente DSP de sdmc:/3ds/dspfirm.cdc. El HLE del emulador
 * ignora su contenido; el DSP real NO: un dummy en consola rompe el audio. */
static int is_emulator(void) {
    s64 out = 0;
    return R_SUCCEEDED(svcGetSystemInfo(&out, 0x20000, 0));   /* Citra/Azahar */
}
static int dspfirm_is_dummy(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t b[64] = {1};
    const size_t n = fread(b, 1, sizeof b, f);
    fclose(f);
    for (size_t i = 0; i < n; i++) if (b[i]) return 0;
    return 1;                       /* todo ceros: lo escribimos nosotros */
}
static void ensure_dspfirm(void) {
    const char *path = "sdmc:/3ds/dspfirm.cdc";
    const int emu = is_emulator();
    if (!emu && dspfirm_is_dummy(path)) {
        remove(path);
        psp_printf("[audio] dspfirm.cdc era un dummy del emulador: borrado.\n");
    }
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return; }
    if (!emu) {
        psp_printf("[audio] FALTA sdmc:/3ds/dspfirm.cdc: dumpea el DSP con la app DSP1.\n");
        return;                     /* ndspInit fallara: juego mudo pero estable */
    }
    mkdir("sdmc:/3ds", 0777);
    f = fopen(path, "wb");
    if (!f) return;
    static const uint8_t z[256];
    for (int i = 0; i < 64; i++) fwrite(z, 1, sizeof z, f);
    fclose(f);
    psp_printf("[audio] dspfirm.cdc dummy creado (el DSP HLE del emulador lo ignora)\n");
}

void ps2_audio_install(void) {
    FILE *noaudio = fopen("sdmc:/djmx_noaudio.flag", "rb");
    if (noaudio) { fclose(noaudio); psp_printf("[audio] desactivado por flag\n"); return; }
    ensure_dspfirm();
    Result rc = ndspInit();
    if (R_FAILED(rc)) {
        psp_printf("[audio] ndspInit fallo 0x%08lX: sin sonido (falta dspfirm?)\n", (unsigned long)rc);
        return;
    }
    g_wmem = (int16_t *)linearAlloc((size_t)NBUF * CHUNK * 2u * sizeof(int16_t));
    if (!g_wmem) { ndspExit(); psp_printf("[audio] sin memoria lineal\n"); return; }
    memset(g_wmem, 0, (size_t)NBUF * CHUNK * 2u * sizeof(int16_t));

    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_NONE);
    ndspChnSetRate(0, 44100.0f);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    float mix[12]; memset(mix, 0, sizeof mix);
    mix[0] = mix[1] = 1.0f;
    ndspChnSetMix(0, mix);

    memset(g_wb, 0, sizeof g_wb);
    for (int i = 0; i < NBUF; i++)
        g_wb[i].data_vaddr = g_wmem + (size_t)i * CHUNK * 2u;

    g_ok = 1;
    ndspSetCallback(ndsp_cb, NULL);
    ndsp_cb(NULL);                      /* ceba la cola con silencio */
    psp_printf("[audio] ndsp OK: 44100 Hz estereo, %u frames/buffer x%d\n",
               (unsigned)CHUNK, NBUF);
}

void ps2_audio_fini(void) {
    if (!g_ok) return;
    g_ok = 0;
    ndspSetCallback(NULL, NULL);
    ndspChnReset(0);
    ndspExit();
    if (g_wmem) { linearFree(g_wmem); g_wmem = NULL; }
}

