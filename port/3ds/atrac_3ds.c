/* atrac_3ds.c -- sceAtrac con decodificación real (BGM audible).
 *
 * Port directo de la sección sceAtrac de port/ps2/ps2_audio.c, que ya hace
 * funcionar la BGM de este mismo juego en PS2:
 *   - ps2_atrac.h decide, llamada a llamada, qué frames se toman del ring del
 *     guest y cuántas muestras recibe el juego (modelo medido del PSP real);
 *   - at3_3ds.cpp decodifica esos frames (FFmpeg AT3/AT3+ vía PPSSPP).
 * Los bytes de los frames se leen en el orden en que el juego los escribió
 * (FIFO de tramos: el buffer de SetData y cada AddStreamData).
 */
#include "hle_3ds.h"
#include "runtime_3ds.h"
#include "ps2_atrac.h"

#include <string.h>

#define ATRAC_FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

#define ATRAC_MAX  6
#define ATRAC_SEGS 32
typedef struct ps2_at3 ps2_at3;
ps2_at3 *ps2_at3_get(int plus, int channels, int bpf, const uint8_t *extra, int extra_size);
void ps2_at3_release(ps2_at3 *d);
void ps2_at3_flush(ps2_at3 *d);
int ps2_at3_decode(ps2_at3 *d, const uint8_t *frame, int16_t *out);

static atrac_t g_atrac[ATRAC_MAX];
static struct {
    ps2_at3 *dec;
    struct { uint32_t addr, len; } seg[ATRAC_SEGS];
    int head, n;
    uint32_t next_off;               /* all-loaded: offset del frame tras el último decodificado */
    int primed;                      /* estado del decoder recién cargado: sin flush */
} g_ap[ATRAC_MAX];
static uint8_t g_at_frame[2048];
static int16_t g_at_pcm[2048 * 2];

static atrac_t *atrac_get(void) {
    const uint32_t id = psp_arg(0);
    if (id >= ATRAC_MAX || !g_atrac[id].used) {
        psp_ret(ATRAC_ERR_BAD_ID);
        return NULL;
    }
    return &g_atrac[id];
}

static void ap_push(int id, uint32_t addr, uint32_t len) {
    if (!len) return;
    if (g_ap[id].n == ATRAC_SEGS) { psp_printf("[atrac] id=%d: FIFO de tramos llena\n", id); return; }
    const int k = (g_ap[id].head + g_ap[id].n) % ATRAC_SEGS;
    g_ap[id].seg[k].addr = addr;
    g_ap[id].seg[k].len = len;
    g_ap[id].n++;
}

/* Los siguientes `bpf` bytes del stream a g_at_frame (ceros si la FIFO se queda corta). */
static void ap_pop(int id, uint32_t bpf) {
    uint32_t got = 0;
    while (got < bpf && g_ap[id].n) {
        uint32_t k = bpf - got;
        if (k > g_ap[id].seg[g_ap[id].head].len) k = g_ap[id].seg[g_ap[id].head].len;
        const uint8_t *p = (const uint8_t *)psp_mem_ptr(g_ap[id].seg[g_ap[id].head].addr, k);
        if (p) memcpy(g_at_frame + got, p, k); else memset(g_at_frame + got, 0, k);
        got += k;
        g_ap[id].seg[g_ap[id].head].addr += k;
        g_ap[id].seg[g_ap[id].head].len -= k;
        if (!g_ap[id].seg[g_ap[id].head].len) { g_ap[id].head = (g_ap[id].head + 1) % ATRAC_SEGS; g_ap[id].n--; }
    }
    if (got < bpf) {
        static uint32_t n;
        if (n++ < 8) psp_printf("[atrac] id=%d: frame corto por %u bytes\n", id, bpf - got);
        memset(g_at_frame + got, 0, bpf - got);
    }
}

/* Decodifica g_at_frame; muestras por canal en g_at_pcm, 0 si falla. */
static int ap_decode(int id) {
    if (!g_ap[id].dec) return 0;
    const int n = ps2_at3_decode(g_ap[id].dec, g_at_frame, g_at_pcm);
    if (n < 0) {
        static uint32_t errs;
        if (errs++ < 8) psp_printf("[atrac] id=%d: error de decode %d\n", id, n);
        return 0;
    }
    return n;
}

static void ap_decode_at(int id, uint32_t file_off) {     /* all-loaded: un frame del buffer */
    const atrac_t *a = &g_atrac[id];
    const uint8_t *p = file_off + a->bpf <= a->buf_size ? (const uint8_t *)psp_mem_ptr(a->buf + file_off, a->bpf) : NULL;
    if (p) memcpy(g_at_frame, p, a->bpf); else memset(g_at_frame, 0, a->bpf);
    g_ap[id].next_off = file_off + a->bpf;
}

static ps2_at3 *ap_open(const atrac_t *a, const uint8_t *p, uint32_t n) {
    const uint8_t *extra = NULL;
    int exn = 0;
    for (uint32_t off = 12; off + 8 <= n; ) {
        const uint32_t id = atrac_rd32(p + off), sz = atrac_rd32(p + off + 4);
        if (id == ATRAC_FOURCC('f', 'm', 't', ' ')) {
            if (sz >= 18 && off + 8 + 18 <= n) {
                exn = (int)atrac_rd16(p + off + 8 + 16);
                extra = p + off + 8 + 18;
                if (off + 8 + 18 + (uint32_t)exn > n) exn = 0;
            }
            break;
        }
        off += 8 + sz + (sz & 1u);
    }
    const int plus = a->spf == 2048;
    return ps2_at3_get(plus, a->channels, (int)a->bpf, plus ? NULL : extra, plus ? 0 : exn);
}

static void hle_AtracSetDataAndGetID(void) {
    const uint32_t buf = psp_arg(0), size = psp_arg(1);
    const uint8_t *p = size ? (const uint8_t *)psp_mem_ptr(buf, size) : NULL;
    if (!p) { psp_ret(ATRAC_ERR_NO_DATA); return; }
    for (uint32_t id = 0; id < ATRAC_MAX; id++) {
        if (g_atrac[id].used) continue;
        const uint32_t e = atrac_set_data(&g_atrac[id], buf, p, size);
        atrac_t *a = &g_atrac[id];
        psp_printf("[atrac] set id=%u size=%u -> 0x%X file=%u bpf=%u ch=%d end=%d state=%d\n",
                   id, size, e, a->file_size, a->bpf, a->channels, a->end, a->state);
        if (!e) {
            g_ap[id].head = g_ap[id].n = 0;
            g_ap[id].dec = ap_open(a, p, size);
            a->consumed = 0;
            /* el frame 0 carga el estado del decoder */
            ap_decode_at((int)id, a->data_off);
            ap_decode((int)id);
            g_ap[id].primed = 1;
            if (atrac_streamed(a)) ap_push((int)id, buf + a->pos, a->valid);
        }
        psp_ret(e ? e : id);
        return;
    }
    psp_ret(0x80630003u);                   /* NO_ATRACID */
}

static void hle_AtracReleaseAtracID(void) {
    atrac_t *a = atrac_get();
    if (!a) return;
    const uint32_t id = psp_arg(0);
    ps2_at3_release(g_ap[id].dec);
    g_ap[id].dec = NULL;
    a->used = 0;
    psp_ret(0);
}

static void hle_AtracDecodeData(void) {
    atrac_t *a = atrac_get();
    if (!a) return;
    const int id = (int)psp_arg(0);
    const int32_t cur0 = a->cur;
    const int32_t skip = (a->ahead + a->cur) % a->spf;
    const uint32_t taken0 = a->consumed;
    uint32_t n, fin;
    const uint32_t e = atrac_decode(a, &n, &fin);

    int frames = 0;                          /* muestras por canal decodificadas en esta llamada */
    if (atrac_streamed(a)) {
        const uint32_t taken = a->consumed - taken0;
        for (uint32_t k = 0; k < taken; k++) {
            if (taken == 2 && k == 0 && !g_ap[id].primed && g_ap[id].dec)
                ps2_at3_flush(g_ap[id].dec);  /* tras un loop: el frame de estado parte limpio */
            ap_pop(id, a->bpf);
            frames = ap_decode(id);
        }
        if (taken) g_ap[id].primed = 0;
    } else if (n) {
        const uint32_t off = atrac_file_offset(a, cur0 - skip);
        if (off != g_ap[id].next_off && off >= a->data_off + a->bpf) {   /* salto: primar */
            ap_decode_at(id, off - a->bpf);
            ap_decode(id);
        }
        ap_decode_at(id, off);
        frames = ap_decode(id);
    }

    if (n && psp_arg(1)) {
        const uint32_t ch = (uint32_t)a->channels;
        int16_t *out = (int16_t *)psp_mem_ptr(psp_arg(1), n * ch * 2u);
        if (out) {
            if (frames >= skip + (int32_t)n) memcpy(out, g_at_pcm + (uint32_t)skip * ch, n * ch * 2u);
            else memset(out, 0, n * ch * 2u);
        }
    }
    /* El juego sigue tras 0x80630023 y lee *samples y *remain igualmente;
     * remain=0 lo manda a recargar el ring, que es lo que un ring vacío pide. */
    if (psp_arg(2)) psp_write32(psp_arg(2), n);
    if (psp_arg(3)) psp_write32(psp_arg(3), fin);
    if (psp_arg(4)) psp_write32(psp_arg(4), e == ATRAC_ERR_EMPTY ? 0u : (uint32_t)atrac_remain(a));
    psp_ret(e);
}

static void hle_AtracAddStreamData(void) {
    atrac_t *a = atrac_get();
    if (!a) return;
    uint32_t o, w, rd;
    atrac_stream_info(a, &o, &w, &rd);
    const uint32_t e = atrac_add(a, psp_arg(1));
    if (!e) ap_push((int)psp_arg(0), a->buf + o, psp_arg(1));
    psp_ret(e);
}

/* Tras el reset, el ring tiene `written` bytes desde su inicio; el primer frame
 * solo carga estado del decoder. */
static void hle_AtracResetPlayPosition(void) {
    atrac_t *a = atrac_get();
    if (!a) return;
    const int id = (int)psp_arg(0);
    const uint32_t e = atrac_reset(a, (int32_t)psp_arg(1), psp_arg(2));
    if (!e && atrac_streamed(a)) {
        g_ap[id].head = g_ap[id].n = 0;
        if (g_ap[id].dec) ps2_at3_flush(g_ap[id].dec);
        const uint8_t *p = (const uint8_t *)psp_mem_ptr(a->buf, a->bpf);
        if (p) { memcpy(g_at_frame, p, a->bpf); ap_decode(id); }
        g_ap[id].primed = 1;
        ap_push(id, a->buf + a->pos, a->valid);
    } else if (!e) {
        g_ap[id].next_off = 0;               /* all-loaded: el próximo decode se prima solo */
    }
    psp_ret(e);
}

static void hle_AtracGetSoundSample(void) {
    atrac_t *a = atrac_get();
    if (!a) return;
    if (psp_arg(1)) psp_write32(psp_arg(1), (uint32_t)a->end);
    if (psp_arg(2)) psp_write32(psp_arg(2), a->has_loop ? (uint32_t)a->loop_start : 0xFFFFFFFFu);
    if (psp_arg(3)) psp_write32(psp_arg(3), a->has_loop ? (uint32_t)a->loop_end : 0xFFFFFFFFu);
    psp_ret(0);
}

static void hle_AtracGetRemainFrame(void) {
    atrac_t *a = atrac_get();
    if (!a) return;
    if (psp_arg(1)) psp_write32(psp_arg(1), (uint32_t)atrac_remain(a));
    psp_ret(0);
}

static void hle_AtracGetStreamDataInfo(void) {
    atrac_t *a = atrac_get();
    if (!a) return;
    uint32_t o, w, rd;
    atrac_stream_info(a, &o, &w, &rd);
    if (psp_arg(1)) psp_write32(psp_arg(1), a->buf + o);
    if (psp_arg(2)) psp_write32(psp_arg(2), w);
    if (psp_arg(3)) psp_write32(psp_arg(3), rd);
    psp_ret(0);
}

static void hle_AtracSetLoopNum(void) {
    atrac_t *a = atrac_get();
    if (!a) return;
    a->loop_num = a->has_loop ? (int32_t)psp_arg(1) : 0;
    psp_ret(0);
}

/* (id, sample, PspBufferInfo *): dos tramos de {write ptr, writable, min write, file pos}. */
static void hle_AtracGetBufferInfoForResetting(void) {
    atrac_t *a = atrac_get();
    if (!a) return;
    const uint32_t p = psp_arg(2);
    uint32_t w, m, f;
    atrac_reset_info(a, (int32_t)psp_arg(1), &w, &m, &f);
    psp_write32(p + 0, a->buf);
    psp_write32(p + 4, w);
    psp_write32(p + 8, m);
    psp_write32(p + 12, f);
    psp_write32(p + 16, a->buf);
    psp_write32(p + 20, 0);
    psp_write32(p + 24, 0);
    psp_write32(p + 28, 0);
    psp_ret(0);
}

static void hle_AtracGetInternalErrorInfo(void) {
    atrac_t *a = atrac_get();
    if (!a) return;
    if (psp_arg(1)) psp_write32(psp_arg(1), 0);
    psp_ret(0);
}

void ps2_atrac_install(void) {
    psp_hle_register(0x7A20E7AF, "sceAtrac3plus", "sceAtracSetDataAndGetID",   hle_AtracSetDataAndGetID);
    psp_hle_register(0x61EB33F5, "sceAtrac3plus", "sceAtracReleaseAtracID",    hle_AtracReleaseAtracID);
    psp_hle_register(0x6A8C3CD5, "sceAtrac3plus", "sceAtracDecodeData",        hle_AtracDecodeData);
    psp_hle_register(0x5D268707, "sceAtrac3plus", "sceAtracGetStreamDataInfo", hle_AtracGetStreamDataInfo);
    psp_hle_register(0x7DB31251, "sceAtrac3plus", "sceAtracAddStreamData",     hle_AtracAddStreamData);
    psp_hle_register(0x9AE849A7, "sceAtrac3plus", "sceAtracGetRemainFrame",    hle_AtracGetRemainFrame);
    psp_hle_register(0xA2BBA8BE, "sceAtrac3plus", "sceAtracGetSoundSample",    hle_AtracGetSoundSample);
    psp_hle_register(0x868120B5, "sceAtrac3plus", "sceAtracSetLoopNum",        hle_AtracSetLoopNum);
    psp_hle_register(0x644E5607, "sceAtrac3plus", "sceAtracResetPlayPosition", hle_AtracResetPlayPosition);
    psp_hle_register(0x2DD3E298, "sceAtrac3plus", "sceAtracGetBufferInfoForResetting", hle_AtracGetBufferInfoForResetting);
    psp_hle_register(0xE88F759B, "sceAtrac3plus", "sceAtracGetInternalErrorInfo", hle_AtracGetInternalErrorInfo);
}
