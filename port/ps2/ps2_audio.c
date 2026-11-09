/* ps2_audio.c -- real time and the silent audio firmware.
 *
 * Clock: the COP0 Count register (294.912 MHz, wraps every 14.56 s) extended by a
 * VBlank interrupt handler. An interrupt rather than sampling at each call because
 * user_main can go a long time without any time call: it loads in sceIoRead loops
 * that never yield. A kernel timer would mean claiming one of the EE timers for what
 * one handler does. Count follows PCSX2's emulated time, like the VBlank itself.
 *
 * Everything that "blocks" goes through wait_until(), a cooperative loop around
 * ps2_sched_yield. Deadlines are in EE cycles, so the sample accounting does not drift.
 */
#include "ps2_audio.h"
#include "ps2_atrac.h"
#include "ps2_sched.h"
#include "ps2_snd.h"
#include "ps2_bga.h"
#include "runtime_min.h"

#include <string.h>
#include <kernel.h>

/* Calibration knob: everything below derives from this. */
#define PS2_EE_HZ      294912000ull
#define AUDIO_RATE     44100ull
/* One PSP vblank (1001/60000 s), times 10 to stay exact in integers. */
#define VBLANK_X10     (PS2_EE_HZ * 1001ull / 6000ull)

/* ---- clock ------------------------------------------------------------- */
static volatile uint32_t g_wraps, g_last;   /* written only by the handler */
static uint64_t g_base;

static inline uint32_t read_count(void) {
    uint32_t c;
    __asm__ __volatile__("mfc0 %0, $9" : "=r"(c));
    return c;
}

/* The real GS vblanks: the game's frame pacing follows these, not an EE-clock model of
 * them, so each frame it draws is on screen for exactly one vblank (two clocks beat
 * against each other and drop or repeat frames: judder that looks like frameskip). */
static volatile uint32_t g_vbl_count;
static volatile uint64_t g_vbl_at;          /* EE cycle of the last one */

static int clock_handler(int cause) {
    const uint32_t c = read_count();
    (void)cause;
    if (c < g_last) g_wraps++;
    g_last = c;
    g_vbl_at = ((uint64_t)g_wraps << 32) | c;
    g_vbl_count++;
    ps2_snd_vblank_isr();
    ExitHandler();
    return 0;
}

static uint64_t ee_cycles(void) {
    uint32_t w, l, c;
    do {                                    /* retried if the handler counted a wrap meanwhile */
        w = g_wraps;
        l = g_last;
        c = read_count();
    } while (w != g_wraps);
    if (c < l) w++;                         /* wrapped, handler not there yet */
    return ((uint64_t)w << 32) | c;
}

static uint64_t cyc2smp(uint64_t c) { return (c - g_base) * AUDIO_RATE / PS2_EE_HZ; }

uint64_t ps2_audio_now_smp(void) { return cyc2smp(ee_cycles()); }   /* ps2_snd.c's pump */

uint64_t ps2_time_us(void) {
    const uint64_t t = ee_cycles() - g_base;
    return t / PS2_EE_HZ * 1000000ull + t % PS2_EE_HZ * 1000000ull / PS2_EE_HZ;
}

/* ---- blocking ----------------------------------------------------------- */
static uint64_t g_wait[PS2_THREAD_MAX];     /* deadline per waiting thread, 0 = free */
static int      g_nwait;

/* If nothing runnable has work, wait for the first deadline here: a round of context
 * switches copies each thread's stack and changes nothing.
 * ponytail: spins on the clock, the EE stays busy. SetAlarm + SleepThread if host CPU matters. */
static void idle_if_all_waiting(void) {
    if (ps2_sched_ready() + 1 != g_nwait) return;
    uint64_t first = ~0ull;
    for (int i = 0; i < PS2_THREAD_MAX; i++)
        if (g_wait[i] && g_wait[i] < first) first = g_wait[i];
    while (ee_cycles() < first) { }
}

/* Always gives the others a turn. Cannot deadlock: nobody is parked, every waiter
 * wakes on the clock alone. */
static void wait_until(uint64_t when) {
    if (!ps2_sched_in_thread()) {           /* module_start: no scheduler to yield to */
        while (ee_cycles() < when) { }
        return;
    }
    int slot = 0;
    while (slot < PS2_THREAD_MAX - 1 && g_wait[slot]) slot++;
    g_wait[slot] = when;
    g_nwait++;
    do {
        ps2_sched_yield();
        if (ee_cycles() >= when) break;
        idle_if_all_waiting();
    } while (ee_cycles() < when);
    g_wait[slot] = 0;
    g_nwait--;
}

/* Interrupt masking, as ps2_hle.c does it, but visible to ps2_audio_preempt. */
static uint32_t g_intr_off;

/* Let expired audio waits run before the next 256-sample (5.8 ms) period.
 * Waiting 20 ms made heavy BGA frames repeatedly starve the stream thread;
 * each underrun moved its channel clock and accumulated audible song drift. */
#define PREEMPT_AFTER (PS2_EE_HZ / 250u)    /* 4 ms: before a 256-sample audio period */
void ps2_audio_preempt(void) {
    if (!g_nwait || g_intr_off || !ps2_sched_in_thread()) return;
    if (ps2_sched_slice_cycles() < PREEMPT_AFTER) return;
    const uint64_t now = ee_cycles();
    for (int i = 0; i < PS2_THREAD_MAX; i++)
        if (g_wait[i] && g_wait[i] <= now) {
            ps2_sched_yield();
            return;
        }
}

static void hle_SuspendIntr(void) {
    const uint32_t prev = g_intr_off;
    g_intr_off = 1;
    psp_ret(prev);
}
static void hle_ResumeIntr(void) {
    const uint32_t prev = g_intr_off;
    g_intr_off = psp_arg(0) & 1u;
    psp_ret(prev);
}

/* ---- ThreadMan / Utils / Rtc time ---------------------------------------- */
static void hle_DelayThread(void) {
    wait_until(ee_cycles() + (uint64_t)psp_arg(0) * (PS2_EE_HZ / 1000ull) / 1000ull);
    psp_ret(0);
}

static void hle_GetSystemTimeWide(void) {
    const uint64_t t = ps2_time_us();
    psp_cpu.r[PSP_REG_V0] = (uint32_t)t;
    psp_cpu.r[PSP_REG_V1] = (uint32_t)(t >> 32);
}

static void hle_GetSystemTime(void) {
    const uint64_t t = ps2_time_us();
    if (psp_arg(0)) {
        psp_write32(psp_arg(0), (uint32_t)t);
        psp_write32(psp_arg(0) + 4, (uint32_t)(t >> 32));
    }
    psp_ret(0);
}

static void hle_GetSystemTimeLow(void) { psp_ret((uint32_t)ps2_time_us()); }

/* sceKernelSysClock2USec(const SceKernelSysClock *clock, u32 *sec, u32 *usec) */
static void hle_SysClock2USec(void) {
    const uint32_t lo = psp_read32(psp_arg(0)), hi = psp_read32(psp_arg(0) + 4);
    const uint64_t t = ((uint64_t)hi << 32) | lo;
    if (psp_arg(1)) psp_write32(psp_arg(1), (uint32_t)(t / 1000000ull));
    if (psp_arg(2)) psp_write32(psp_arg(2), (uint32_t)(t % 1000000ull));
    psp_ret(0);
}

/* ponytail: no RTC read, the wall clock is 2008-01-01 12:00 plus uptime and the date
 * does not roll past the month. The game only seeds from it and stamps saves. */
#define WALL_EPOCH 1199188800u              /* 2008-01-01 12:00:00 UTC */

static void hle_LibcClock(void) { psp_ret((uint32_t)ps2_time_us()); }

static void hle_LibcTime(void) {
    const uint32_t t = WALL_EPOCH + (uint32_t)(ps2_time_us() / 1000000ull);
    if (psp_arg(0)) psp_write32(psp_arg(0), t);
    psp_ret(t);
}

static void hle_LibcGettimeofday(void) {
    const uint64_t t = ps2_time_us();
    if (psp_arg(0)) {
        psp_write32(psp_arg(0), WALL_EPOCH + (uint32_t)(t / 1000000ull));
        psp_write32(psp_arg(0) + 4, (uint32_t)(t % 1000000ull));
    }
    if (psp_arg(1)) {
        psp_write32(psp_arg(1), 0);
        psp_write32(psp_arg(1) + 4, 0);
    }
    psp_ret(0);
}

/* sceRtcGetCurrentClockLocalTime(ScePspDateTime *, int tz) */
static void hle_RtcLocalTime(void) {
    const uint32_t p = psp_arg(0);
    const uint64_t t = ps2_time_us();
    const uint32_t s = (uint32_t)(t / 1000000ull) + 12u * 3600u;
    psp_write16(p + 0, 2008);
    psp_write16(p + 2, 1);
    psp_write16(p + 4, (uint16_t)(1u + s / 86400u));
    psp_write16(p + 6, (uint16_t)(s / 3600u % 24u));
    psp_write16(p + 8, (uint16_t)(s / 60u % 60u));
    psp_write16(p + 10, (uint16_t)(s % 60u));
    psp_write32(p + 12, (uint32_t)(t % 1000000ull));
    psp_ret(0);
}

/* ---- display -------------------------------------------------------------
 * The vcount is the number of real vblank periods since install, so it moves on its
 * own however little the caller does; that is what the boot's wait on it needs. */
static uint32_t vcount_now(void) { return g_vbl_count + 1u; }

static void hle_WaitVblank(void) {
    /* Sleep to just before the predicted vblank, then yield until the handler has run. */
    const uint32_t v = g_vbl_count;
    wait_until(g_vbl_at + VBLANK_X10 / 10ull - PS2_EE_HZ / 4000ull);
    while (g_vbl_count == v) ps2_sched_yield();
    { extern void ps2_sched_frame(uint32_t); static uint32_t last;   /* TEMP [slow] */
      ps2_sched_frame(last ? g_vbl_count - last : 1u); last = g_vbl_count; }
    ps2_snd_report(cyc2smp(ee_cycles()));
    psp_ret(0);
}

static void hle_GetVcount(void) {
    static uint32_t last;
    const uint32_t v = vcount_now();
    if (v == last) ps2_sched_yield();       /* the caller may be spinning on it */
    last = v;
    psp_ret(v);
}

/* The real call returns a float, in $f0 (the movie player divides by it at 0x8B5BC). */
static void hle_GetFramePerSec(void) { psp_cpu.f[0] = 59.94005f; }

/* ---- sceAudio ------------------------------------------------------------- */
#define AUDIO_ERR_CHANNEL  0x80260003u
#define AUDIO_ERR_BUSY     0x80260002u
#define AUDIO_ERR_NOT_FOUND 0x80260005u
#define AUDIO_ERR_SIZE     0x80260006u
#define AUDIO_ERR_FORMAT   0x80260007u
#define AUDIO_ERR_NOT_RES  0x80260008u
#define AUDIO_CHANNELS     8

static struct { int reserved, mono; uint32_t samples; uint64_t busy_until, next_smp; } g_ch[AUDIO_CHANNELS];

static int audio_size_ok(uint32_t n) { return n >= 64 && n <= 65472 && !(n & 63); }

/* The hardware queue holds one buffer: a blocking output waits for the previous one
 * to finish, then queues and returns, so the caller is one buffer ahead of playback.
 * The PCM goes into the mix (ps2_snd.c) at the sample where it starts: right after the
 * channel's previous buffer, or now if the channel had run dry.
 *
 * "Dry" allows for the cooperative scheduler: a PSP audio thread preempts the game,
 * here it waits for the game's frame to end, so a buffer can come a few ms after the
 * previous one finished. While that is less than the mix lag (the previous buffer's end
 * is not sent yet) it still goes right after it, and the channel's clock follows the
 * samples: no gap in the sound, and the thread catches up by not blocking. */
#define AUDIO_LATE_OK  (SND_LAG - 128u)
static uint64_t smp2cyc(uint64_t s) { return g_base + s * PS2_EE_HZ / AUDIO_RATE; }

static void audio_output(int blocking, int panned) {
    const uint32_t ch = psp_arg(0);
    if (ch >= AUDIO_CHANNELS) { psp_ret(AUDIO_ERR_CHANNEL); return; }
    if (!g_ch[ch].reserved)   { psp_ret(AUDIO_ERR_NOT_RES); return; }

    uint64_t start = ee_cycles();
    int contiguous = 0;
    if (g_ch[ch].busy_until > start) {
        if (!blocking) { psp_ret(AUDIO_ERR_BUSY); return; }
        start = g_ch[ch].busy_until;
        wait_until(start);
        contiguous = 1;
    } else if (g_ch[ch].next_smp && cyc2smp(start) < g_ch[ch].next_smp + AUDIO_LATE_OK) {
        contiguous = 1;                      /* late, but its place is not sent yet */
    }
    const uint32_t n = g_ch[ch].samples;
    g_ch[ch].busy_until = contiguous ? smp2cyc(g_ch[ch].next_smp + n)
                                     : start + n * PS2_EE_HZ / AUDIO_RATE;

    const int32_t lv = (int32_t)(psp_arg(1) & 0xFFFFu);
    const int32_t rv = panned ? (int32_t)(psp_arg(2) & 0xFFFFu) : lv;
    const uint32_t buf = psp_arg(panned ? 3 : 2);
    const int16_t *pcm = buf ? (const int16_t *)psp_mem_ptr(buf, n * (g_ch[ch].mono ? 2u : 4u)) : NULL;
    const uint64_t now = cyc2smp(ee_cycles());
    const uint64_t at = contiguous ? g_ch[ch].next_smp : cyc2smp(start);
    if (pcm && !strcmp(ps2_sched_current_name(), "stream_sound_poll_thread")) {
        if (!contiguous && g_ch[ch].next_smp && at > g_ch[ch].next_smp)
            psp_printf("[audio] stream gap=%u samples at %llu us\n",
                       (uint32_t)(at - g_ch[ch].next_smp), (unsigned long long)ps2_time_us());
    }
    g_ch[ch].next_smp = at + n;
    ps2_snd_mix(at, pcm, n, !g_ch[ch].mono, lv, rv, now);
    if (pcm && !strcmp(ps2_sched_current_name(), "stream_sound_poll_thread")) ps2_bga_audio_samples(n);
    psp_ret(n);
}

static void hle_AudioOutputBlocking(void)       { audio_output(1, 0); }
static void hle_AudioOutputPannedBlocking(void) { audio_output(1, 1); }
static void hle_AudioOutputPanned(void)         { audio_output(0, 1); }

static void hle_AudioChReserve(void) {
    int32_t ch = (int32_t)psp_arg(0);
    if (ch < 0) {
        for (ch = 0; ch < AUDIO_CHANNELS && g_ch[ch].reserved; ch++) { }
        if (ch == AUDIO_CHANNELS) { psp_ret(AUDIO_ERR_NOT_FOUND); return; }
    }
    if (ch >= AUDIO_CHANNELS || g_ch[ch].reserved) { psp_ret(AUDIO_ERR_CHANNEL); return; }
    if (!audio_size_ok(psp_arg(1))) { psp_ret(AUDIO_ERR_SIZE); return; }
    if (psp_arg(2) != 0 && psp_arg(2) != 0x10u) { psp_ret(AUDIO_ERR_FORMAT); return; }
    g_ch[ch].reserved = 1;
    g_ch[ch].samples = psp_arg(1);
    g_ch[ch].mono = psp_arg(2) == 0x10u;
    g_ch[ch].busy_until = 0;
    g_ch[ch].next_smp = 0;
    psp_printf("[audio] reserve ch=%d samples=%u fmt=0x%X\n", ch, psp_arg(1), psp_arg(2));
    psp_ret((uint32_t)ch);
}

static void hle_AudioChRelease(void) {
    const uint32_t ch = psp_arg(0);
    if (ch >= AUDIO_CHANNELS) { psp_ret(AUDIO_ERR_CHANNEL); return; }
    if (!g_ch[ch].reserved)   { psp_ret(AUDIO_ERR_NOT_RES); return; }
    g_ch[ch].reserved = 0;
    g_ch[ch].next_smp = 0;
    psp_ret(0);
}

static void hle_AudioSetChannelDataLen(void) {
    const uint32_t ch = psp_arg(0);
    if (ch >= AUDIO_CHANNELS) { psp_ret(AUDIO_ERR_CHANNEL); return; }
    if (!g_ch[ch].reserved)   { psp_ret(AUDIO_ERR_NOT_RES); return; }
    if (!audio_size_ok(psp_arg(1))) { psp_ret(AUDIO_ERR_SIZE); return; }
    g_ch[ch].samples = psp_arg(1);
    psp_ret(0);
}

static void hle_AudioGetChannelRestLength(void) {
    const uint32_t ch = psp_arg(0);
    if (ch >= AUDIO_CHANNELS) { psp_ret(AUDIO_ERR_CHANNEL); return; }
    if (!g_ch[ch].reserved)   { psp_ret(AUDIO_ERR_NOT_RES); return; }
    const uint64_t now = ee_cycles();
    psp_ret(g_ch[ch].busy_until > now ? (uint32_t)((g_ch[ch].busy_until - now) * AUDIO_RATE / PS2_EE_HZ) : 0u);
}

/* (ch, format): 0 stereo, 0x10 mono. */
static void hle_AudioChangeChannelConfig(void) {
    const uint32_t ch = psp_arg(0);
    if (ch >= AUDIO_CHANNELS) { psp_ret(AUDIO_ERR_CHANNEL); return; }
    if (!g_ch[ch].reserved)   { psp_ret(AUDIO_ERR_NOT_RES); return; }
    g_ch[ch].mono = psp_arg(1) == 0x10u;
    psp_ret(0);
}

/* Volume is given again with every output. */
static void hle_AudioChOk(void) {
    const uint32_t ch = psp_arg(0);
    if (ch >= AUDIO_CHANNELS) { psp_ret(AUDIO_ERR_CHANNEL); return; }
    psp_ret(g_ch[ch].reserved ? 0u : AUDIO_ERR_NOT_RES);
}

/* ---- sceAtrac -------------------------------------------------------------
 * ps2_atrac.h decides, call by call, which frames are taken from the guest ring and how
 * many samples the guest gets; this part decodes those frames (ps2_at3.cpp) and hands
 * over samples [skip, skip + got) of the frame, as the PSP does.
 *
 * The frames' bytes are read in the order the game wrote them (a FIFO of guest spans:
 * the SetData buffer, then every AddStreamData at the offset GetStreamDataInfo gave).
 * Reading buf+pos straight would be wrong where the first buffer spills past the
 * frame-aligned ring end (ps2_atrac.h).
 *
 * Frames that only load decoder state (frame 0 at SetData, the frame before a loop or
 * reset point) are decoded and their output dropped. */
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
    uint32_t next_off;               /* all-loaded: file offset of the frame after the last decoded */
    int primed;                      /* decoder state just loaded (SetData, reset): no flush */
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
    if (g_ap[id].n == ATRAC_SEGS) { psp_printf("[atrac] id=%d: span FIFO full\n", id); return; }
    const int k = (g_ap[id].head + g_ap[id].n) % ATRAC_SEGS;
    g_ap[id].seg[k].addr = addr;
    g_ap[id].seg[k].len = len;
    g_ap[id].n++;
}

/* Next `bpf` bytes of the stream into g_at_frame (zeros where the FIFO ran short). */
static void ap_pop(int id, uint32_t bpf) {
    uint32_t got = 0;
    while (got < bpf && g_ap[id].n) {
        typeof(g_ap[0].seg[0]) *s = &g_ap[id].seg[g_ap[id].head];
        uint32_t k = bpf - got;
        if (k > s->len) k = s->len;
        const uint8_t *p = (const uint8_t *)psp_mem_ptr(s->addr, k);
        if (p) memcpy(g_at_frame + got, p, k); else memset(g_at_frame + got, 0, k);
        got += k;
        s->addr += k;
        s->len -= k;
        if (!s->len) { g_ap[id].head = (g_ap[id].head + 1) % ATRAC_SEGS; g_ap[id].n--; }
    }
    if (got < bpf) {
        static uint32_t n;
        if (n++ < 8) psp_printf("[atrac] id=%d: frame short by %u bytes\n", id, bpf - got);
        memset(g_at_frame + got, 0, bpf - got);
    }
}

/* Decode the frame in g_at_frame; returns samples per channel in g_at_pcm, 0 on error. */
static int ap_decode(int id) {
    if (!g_ap[id].dec) return 0;
    const int n = ps2_at3_decode(g_ap[id].dec, g_at_frame, g_at_pcm);
    if (n < 0) {
        static uint32_t errs;
        if (errs++ < 8) psp_printf("[atrac] id=%d: decode error %d\n", id, n);
        return 0;
    }
    return n;
}

static void ap_decode_at(int id, uint32_t file_off) {     /* all-loaded: a frame of the buffer */
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
    ps2_bga_audio_reset();
    const uint32_t buf = psp_arg(0), size = psp_arg(1);
    const uint8_t *p = size ? (const uint8_t *)psp_mem_ptr(buf, size) : NULL;
    if (!p) { psp_ret(ATRAC_ERR_NO_DATA); return; }
    for (uint32_t id = 0; id < ATRAC_MAX; id++) {
        if (g_atrac[id].used) continue;
        const uint32_t e = atrac_set_data(&g_atrac[id], buf, p, size);
        atrac_t *a = &g_atrac[id];
        psp_printf("[atrac] set id=%u size=%u -> 0x%X file=%u bpf=%u ch=%d end=%d delay=%d state=%d at %llu us\n",
                   id, size, e, a->file_size, a->bpf, a->channels, a->end, a->delay, a->state,
                   (unsigned long long)ps2_time_us());
        if (!e) {
            g_ap[id].head = g_ap[id].n = 0;
            g_ap[id].dec = ap_open(a, p, size);
            a->consumed = 0;
            /* frame 0 loads decoder state */
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

    int frames = 0;                          /* samples per channel decoded for this call */
    if (atrac_streamed(a)) {
        const uint32_t taken = a->consumed - taken0;
        for (uint32_t k = 0; k < taken; k++) {
            if (taken == 2 && k == 0 && !g_ap[id].primed && g_ap[id].dec)
                ps2_at3_flush(g_ap[id].dec);  /* after a loop: the state-only frame starts clean */
            ap_pop(id, a->bpf);
            frames = ap_decode(id);
        }
        if (taken) g_ap[id].primed = 0;
    } else if (n) {
        const uint32_t off = atrac_file_offset(a, cur0 - skip);
        if (off != g_ap[id].next_off && off >= a->data_off + a->bpf) {   /* jumped: prime */
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
    /* The game keeps going after 0x80630023 and reads *samples and *remain anyway
     * (0x7F100, 0x7F1AC). A PSP leaves *remain alone there, which would be stale stack
     * for this caller; 0 sends it to the refill path, which is what an empty ring wants. */
    if (psp_arg(2)) psp_write32(psp_arg(2), n);
    if (psp_arg(3)) psp_write32(psp_arg(3), fin);
    if (psp_arg(4)) psp_write32(psp_arg(4), e == ATRAC_ERR_EMPTY ? 0u : (uint32_t)atrac_remain(a));
    if (e == ATRAC_ERR_AFTER_END) {
        static uint32_t logged;
        if (logged < 16) {
            logged++;
            psp_printf("[atrac] end id=%u cur=%d end=%d at %llu us\n", psp_arg(0), a->cur, a->end,
                       (unsigned long long)ps2_time_us());
        }
    }
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

/* After the reset the ring holds `written` bytes from its start; the first frame only
 * loads decoder state. */
static void hle_AtracResetPlayPosition(void) {
    atrac_t *a = atrac_get();
    if (!a) return;
    const int id = (int)psp_arg(0);
    const uint32_t e = atrac_reset(a, (int32_t)psp_arg(1), psp_arg(2));
    if (!e) ps2_bga_audio_seek(psp_arg(1));
    if (!e && atrac_streamed(a)) {
        g_ap[id].head = g_ap[id].n = 0;
        if (g_ap[id].dec) ps2_at3_flush(g_ap[id].dec);
        const uint8_t *p = (const uint8_t *)psp_mem_ptr(a->buf, a->bpf);
        if (p) { memcpy(g_at_frame, p, a->bpf); ap_decode(id); }
        g_ap[id].primed = 1;
        ap_push(id, a->buf + a->pos, a->valid);
    } else if (!e) {
        g_ap[id].next_off = 0;               /* all-loaded: the next decode primes itself */
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
    /* ponytail: a PSP refuses a track without loop info. All 554 of this game have it. */
    a->loop_num = a->has_loop ? (int32_t)psp_arg(1) : 0;
    psp_ret(0);
}

/* (id, sample, PspBufferInfo *): two spans of {write ptr, writable, min write, file pos}. */
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

/* ---- sceSas --------------------------------------------------------------
 * The voices really play: VAG ADPCM (or s16 PCM) from guest memory, pitch 0x1000 = 1.0
 * with linear interpolation, per-voice left/right volume (0..0x1000), mixed into the
 * grain the game then outputs on its sceAudio channel, with the game's ADSR envelopes.
 * Reverb (__sceSasRev*) is accepted and not applied: the game sets the effect type to
 * -1 (off) at init (0xAAB00) and never changes it.
 * ponytail: noise voices are silent (none keyed in the sessions seen). */
#define SAS_ERR_ADDRESS    0x80420005u
#define SAS_ERR_VOICE      0x80420010u
#define SAS_ERR_PITCH      0x80420012u
#define SAS_ERR_SIZE       0x80420014u
#define SAS_ERR_LOOP       0x80420015u
#define SAS_ERR_STATE      0x80420016u
#define SAS_ERR_NOTINIT    0x80420100u
#define SAS_VOICES         32
#define SAS_GRAIN_MAX      1024u

enum { SRC_NONE = 0, SRC_VAG, SRC_PCM, SRC_NOISE };
#define SAS_ENV_MAX 0x40000000
enum { ENV_ATTACK = 0, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE, ENV_OFF };
enum { CURVE_LIN_UP = 0, CURVE_LIN_DOWN, CURVE_LIN_BENT, CURVE_EXP_DOWN, CURVE_EXP_UP, CURVE_DIRECT };

typedef struct {
    uint8_t  on, key_off, paused, endless, src;
    int32_t  pitch, vl, vr;
    uint32_t addr, size;          /* guest source: VAG bytes or PCM samples */
    int32_t  pcm_loop;            /* PCM loop start sample, -1 none */
    /* playback */
    uint32_t blk, loop_blk;       /* VAG: next block, loop start block */
    int16_t  dec[28];
    int32_t  dpos, h1, h2;
    int      last;                /* VAG: the decoded block was the final one */
    uint32_t ppos;                /* PCM: next sample */
    uint32_t frac;                /* 12-bit fraction between prev and cur */
    int32_t  prev, cur;
    int32_t  fade;                /* key off, no envelope: samples of fade left */
    /* envelope (sas_simple_env / SetADSR...) */
    uint8_t  has_env, env, a_type, d_type, s_type, r_type;
    int32_t  height, a_rate, d_rate, s_rate, r_rate, sl;
} sas_voice;

static sas_voice g_sv[SAS_VOICES];
static uint32_t g_sas_core, g_sas_grain, g_sas_mode;

/* Check the core and (optionally) voice args; on failure $v0 holds the error. */
static int sas_core_ok(void) {
    if (!psp_arg(0) || (psp_arg(0) & 63u)) { psp_ret(SAS_ERR_ADDRESS); return 0; }
    if (psp_arg(0) != g_sas_core)          { psp_ret(SAS_ERR_NOTINIT); return 0; }
    return 1;
}

static int sas_voice_arg(void) {
    if (!sas_core_ok()) return -1;
    if (psp_arg(1) >= SAS_VOICES) { psp_ret(SAS_ERR_VOICE); return -1; }
    return (int)psp_arg(1);
}

static void hle_SasInit(void) {
    if (!psp_arg(0) || (psp_arg(0) & 63u)) { psp_ret(SAS_ERR_ADDRESS); return; }
    memset(g_sv, 0, sizeof g_sv);
    for (int i = 0; i < SAS_VOICES; i++) {
        g_sv[i].pitch = 0x1000;
        g_sv[i].vl = g_sv[i].vr = 0x1000;
        /* default curves (the game sets rates with __sceSasSetADSR but never the modes) */
        g_sv[i].a_type = CURVE_LIN_UP;
        g_sv[i].d_type = g_sv[i].s_type = g_sv[i].r_type = CURVE_LIN_DOWN;
    }
    g_sas_core = psp_arg(0);
    g_sas_grain = psp_arg(1);
    g_sas_mode = psp_arg(3);
    psp_printf("[sas] init core=0x%08X grain=%u voices=%u mode=%u rate=%u\n",
               psp_arg(0), psp_arg(1), psp_arg(2), psp_arg(3), psp_arg(4));
    psp_ret(0);
}

static void sas_set_source(int v, int src, uint32_t addr, uint32_t size, int endless) {
    sas_voice *s = &g_sv[v];
    s->on = 0;
    s->key_off = 0;
    s->src = (uint8_t)src;
    s->addr = addr;
    s->size = size;
    s->endless = (uint8_t)endless;
}

/* (core, voice, vag, size, loop) */
static void hle_SasSetVoice(void) {
    const int v = sas_voice_arg();
    if (v < 0) return;
    const uint32_t size = psp_arg(3), loop = psp_arg(4);
    if (!size || (size & 15u) || size > 0x7FFFFFF0u) { psp_ret(SAS_ERR_SIZE); return; }
    if (loop > 1u) { psp_ret(SAS_ERR_LOOP); return; }
    if (!psp_arg(2) || !psp_mem_ptr(psp_arg(2), size)) { psp_ret(SAS_ERR_ADDRESS); return; }
    /* The block flags decide, not the loop argument alone: a voice ends at a flag-7 block or
     * at the end of its data, and only loops when the game asked for it AND the data marks
     * a loop start (6) followed by a repeat (3). Looped sounds without markers still end. */
    const uint8_t *vag = (const uint8_t *)psp_mem_ptr(psp_arg(2), size);
    int endless = 0, loop_start = 0;
    for (uint32_t b = 0; b < size / 16u; b++) {
        const uint8_t f = vag[b * 16u + 1u];
        if (f == 7u) break;
        if (f == 6u) loop_start = 1;
        if (f == 3u && loop && loop_start) { endless = 1; break; }
    }
    sas_set_source(v, SRC_VAG, psp_arg(2), size, endless);
    psp_ret(0);
}

/* (core, voice, pcm, samples, loop start or -1) */
static void hle_SasSetVoicePCM(void) {
    const int v = sas_voice_arg();
    if (v < 0) return;
    const int32_t n = (int32_t)psp_arg(3);
    if (n <= 0) { psp_ret(SAS_ERR_SIZE); return; }
    if (!psp_mem_ptr(psp_arg(2), (uint32_t)n * 2u)) { psp_ret(SAS_ERR_ADDRESS); return; }
    sas_set_source(v, SRC_PCM, psp_arg(2), (uint32_t)n, (int32_t)psp_arg(4) >= 0);
    g_sv[v].pcm_loop = (int32_t)psp_arg(4);
    psp_ret(0);
}

static void hle_SasSetNoise(void) {
    const int v = sas_voice_arg();
    if (v < 0) return;
    sas_set_source(v, SRC_NOISE, 0, 0, 1);
    psp_ret(0);
}

static void hle_SasSetPitch(void) {
    const int v = sas_voice_arg();
    if (v < 0) return;
    if ((int32_t)psp_arg(2) < 1 || (int32_t)psp_arg(2) > 0x4000) { psp_ret(SAS_ERR_PITCH); return; }
    g_sv[v].pitch = (int32_t)psp_arg(2);
    psp_ret(0);
}

/* (core, voice, left, right, effect left, effect right) */
static void hle_SasSetVolume(void) {
    const int v = sas_voice_arg();
    if (v < 0) return;
    g_sv[v].vl = (int32_t)psp_arg(2);
    g_sv[v].vr = (int32_t)psp_arg(3);
    psp_ret(0);
}

static int32_t sas_next(sas_voice *s);

static void hle_SasSetKeyOn(void) {
    const int v = sas_voice_arg();
    if (v < 0) return;
    sas_voice *s = &g_sv[v];
    if (s->paused || s->src == SRC_NONE) { psp_ret(SAS_ERR_STATE); return; }
    s->blk = 0; s->loop_blk = 0; s->dpos = 28; s->h1 = s->h2 = 0; s->last = 0;
    s->ppos = 0; s->frac = 0; s->fade = 0;
    s->env = ENV_ATTACK;
    s->height = s->has_env ? 0 : SAS_ENV_MAX;
    s->on = 1;
    s->key_off = 0;
    s->prev = 0;
    s->cur = sas_next(s);
    { static uint32_t n; if (n < 40) { n++;
        psp_printf("[sas] keyon v=%d src=%u size=%u pitch=0x%X vol=%d/%d endless=%u\n",
                   v, s->src, s->size, s->pitch, s->vl, s->vr, s->endless); } }
    psp_ret(0);
}

static void hle_SasSetKeyOff(void) {
    const int v = sas_voice_arg();
    if (v < 0) return;
    sas_voice *s = &g_sv[v];
    if (s->on && !s->key_off) {
        s->key_off = 1;
        if (s->has_env) s->env = ENV_RELEASE;
        else s->fade = (int32_t)g_sas_grain;
    }
    psp_ret(0);
}

/* Next source sample of a voice; clears `on` when the data is over. */
static int32_t sas_next(sas_voice *s) {
    static const int32_t F[5][2] = { {0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60} };
    if (s->src == SRC_PCM) {
        if (s->ppos >= s->size) {
            if (s->pcm_loop >= 0 && (uint32_t)s->pcm_loop < s->size) s->ppos = (uint32_t)s->pcm_loop;
            else { s->on = 0; return 0; }
        }
        const int16_t *p = (const int16_t *)psp_mem_ptr(s->addr + s->ppos * 2u, 2);
        s->ppos++;
        return p ? *p : 0;
    }
    if (s->src != SRC_VAG) return 0;
    if (s->dpos >= 28) {
        if (s->last || s->blk >= s->size / 16u) { s->on = 0; return 0; }
        const uint8_t *b = (const uint8_t *)psp_mem_ptr(s->addr + s->blk * 16u, 16);
        if (!b || b[1] == 7u) { s->on = 0; return 0; }
        const uint8_t flag = b[1];
        int shift = b[0] & 15, filt = b[0] >> 4;
        if (shift > 12) shift = 9;
        if (filt > 4) filt = 0;
        if (flag & 4u) s->loop_blk = s->blk;
        for (int i = 0; i < 28; i++) {
            const int nib = (b[2 + i / 2] >> ((i & 1) * 4)) & 15;
            int32_t x = ((int32_t)(int16_t)(nib << 12)) >> shift;
            x += (s->h1 * F[filt][0] + s->h2 * F[filt][1]) >> 6;   /* SPU2/PCSX2 rounding */
            x = x > 32767 ? 32767 : x < -32768 ? -32768 : x;
            s->h2 = s->h1;
            s->h1 = x;
            s->dec[i] = (int16_t)x;
        }
        s->dpos = 0;
        s->blk++;
        if (flag & 1u) {                      /* loop end: repeat or stop after this block */
            if ((flag & 2u) && s->endless) s->blk = s->loop_blk;
            else s->last = 1;
        }
    }
    return s->dec[s->dpos++];
}

/* ADSR, per output sample, as the PSP's sas: height 0..0x40000000 (1.0).
 * Curves: 0 linear up, 1 linear down, 2 linear bent (up, a quarter as fast above 3/4),
 * 3 exponential down, 4 exponential up, 5 direct (height = rate).
 * Rates of the linear curves are per-sample steps; of the exponential ones, the
 * fraction (x / 2^32) of the distance that goes per sample.
 * A voice the game never gave an envelope keeps full height (key off: a one-grain fade). */

static int32_t simple_rate(int n) {
    n &= 0x7F;
    if (n == 0x7F) return 0;
    const int32_t r = (int32_t)(((uint32_t)(7 - (n & 3)) << 26) >> (n >> 2));
    return r ? r : 1;
}
static int32_t exponent_rate(int n) {
    n &= 0x7F;
    if (n == 0x7F) return 0;
    const int32_t r = (int32_t)(((uint32_t)(7 - (n & 3)) << 24) >> (n >> 2));
    return r ? r : 1;
}

/* SetSimpleADSR: the SPU-style pair of 16-bit words. */
static void sas_simple_env(sas_voice *s, uint32_t e1, uint32_t e2) {
    s->a_rate = simple_rate((int)(e1 >> 8));
    s->a_type = (e1 & 0x8000u) ? CURVE_LIN_BENT : CURVE_LIN_UP;
    const uint32_t d = (e1 >> 4) & 15u;
    s->d_rate = d ? (int32_t)(0x80000000u >> d) : 0x7FFFFFFF;
    s->d_type = CURVE_EXP_DOWN;
    s->sl = (int32_t)(((e1 & 15u) + 1u) << 26);
    switch (e2 >> 13) {
    case 2:  s->s_type = CURVE_LIN_DOWN; break;
    case 4:  s->s_type = CURVE_LIN_BENT; break;
    case 6:  s->s_type = CURVE_EXP_DOWN; break;
    default: s->s_type = CURVE_LIN_UP; break;
    }
    s->s_rate = s->s_type == CURVE_EXP_DOWN ? exponent_rate((int)(e2 >> 6)) : simple_rate((int)(e2 >> 6));
    s->r_type = (e2 & 0x20u) ? CURVE_EXP_DOWN : CURVE_LIN_DOWN;
    const uint32_t r = e2 & 31u;
    if (r == 31u) s->r_rate = 0;
    else if (s->r_type == CURVE_LIN_DOWN) s->r_rate = r == 30u ? 0x40000000 : r == 29u ? 1 : (int32_t)(0x10000000u >> r);
    else s->r_rate = r ? (int32_t)(0x80000000u >> r) : 0x7FFFFFFF;
    s->has_env = 1;
}

static void env_walk(sas_voice *s, int type, int32_t rate) {
    int64_t h = s->height;
    switch (type) {
    case CURVE_LIN_UP:   h += rate; break;
    case CURVE_LIN_DOWN: h -= rate; break;
    case CURVE_LIN_BENT: h += h <= (int64_t)SAS_ENV_MAX * 3 / 4 ? rate : rate / 4; break;
    case CURVE_EXP_DOWN: h -= ((h * (int64_t)(uint32_t)rate) >> 32) + 1; break;
    case CURVE_EXP_UP:   h += (((int64_t)SAS_ENV_MAX - h) * (int64_t)(uint32_t)rate >> 32) + 1; break;
    case CURVE_DIRECT:   h = rate; break;
    }
    s->height = h > 0x7FFFFFFF ? 0x7FFFFFFF : (int32_t)h;
}

/* One sample of envelope; clears `on` when the release reaches 0. */
static void env_step(sas_voice *s) {
    switch (s->env) {
    case ENV_ATTACK:
        env_walk(s, s->a_type, s->a_rate);
        if (s->height >= SAS_ENV_MAX || s->height < 0) { s->height = SAS_ENV_MAX; s->env = ENV_DECAY; }
        break;
    case ENV_DECAY:
        env_walk(s, s->d_type, s->d_rate);
        if (s->height <= s->sl) { if (s->height < 0) s->height = 0; s->env = ENV_SUSTAIN; }
        break;
    case ENV_SUSTAIN:
        env_walk(s, s->s_type, s->s_rate);
        if (s->height > SAS_ENV_MAX) s->height = SAS_ENV_MAX;
        if (s->height <= 0) { s->height = 0; s->env = ENV_RELEASE; }
        break;
    case ENV_RELEASE:
        env_walk(s, s->r_type, s->r_rate);
        if (s->height <= 0) { s->height = 0; s->env = ENV_OFF; s->on = 0; }
        break;
    default:
        s->on = 0;
        break;
    }
}

/* (core, voice, flag, a, d, s, r): flag bits 1/2/4/8 pick which are set */
static void hle_SasSetADSR(void) {
    const int v = sas_voice_arg();
    if (v < 0) return;
    sas_voice *s = &g_sv[v];
    const uint32_t f = psp_arg(2);
    if (f & 1u) s->a_rate = (int32_t)psp_arg(3);
    if (f & 2u) s->d_rate = (int32_t)psp_arg(4);
    if (f & 4u) s->s_rate = (int32_t)psp_arg(5);
    if (f & 8u) s->r_rate = (int32_t)psp_arg(6);
    s->has_env = 1;
    { static uint32_t n; if (n < 6) { n++; psp_printf("[sas] ADSR v=%d flag=%u a=0x%X d=0x%X s=0x%X r=0x%X\n",
        v, f, psp_arg(3), psp_arg(4), psp_arg(5), psp_arg(6)); } }
    psp_ret(0);
}

static void hle_SasSetADSRmode(void) {
    const int v = sas_voice_arg();
    if (v < 0) return;
    sas_voice *s = &g_sv[v];
    const uint32_t f = psp_arg(2);
    if (f & 1u) s->a_type = (uint8_t)psp_arg(3);
    if (f & 2u) s->d_type = (uint8_t)psp_arg(4);
    if (f & 4u) s->s_type = (uint8_t)psp_arg(5);
    if (f & 8u) s->r_type = (uint8_t)psp_arg(6);
    s->has_env = 1;
    { static uint32_t n; if (n < 6) { n++; psp_printf("[sas] ADSRmode v=%d flag=%u a=%u d=%u s=%u r=%u\n",
        v, f, psp_arg(3), psp_arg(4), psp_arg(5), psp_arg(6)); } }
    psp_ret(0);
}

static void hle_SasSetSL(void) {
    const int v = sas_voice_arg();
    if (v < 0) return;
    g_sv[v].sl = (int32_t)psp_arg(2);
    g_sv[v].has_env = 1;
    psp_ret(0);
}

static void hle_SasSetSimpleADSR(void) {
    const int v = sas_voice_arg();
    if (v < 0) return;
    sas_simple_env(&g_sv[v], psp_arg(2) & 0xFFFFu, psp_arg(3) & 0xFFFFu);
    { static uint32_t n; if (n < 6) { n++; psp_printf("[sas] SimpleADSR v=%d e1=0x%04X e2=0x%04X\n", v, psp_arg(2), psp_arg(3)); } }
    psp_ret(0);
}

/* Mix every sounding voice into acc (n stereo frames). */
static void sas_mix(int32_t (*acc)[2], uint32_t n) {
    for (int v = 0; v < SAS_VOICES; v++) {
        sas_voice *s = &g_sv[v];
        if (!s->on || s->paused) continue;
        if (s->src == SRC_NOISE) { if (s->key_off) s->on = 0; continue; }
        const int32_t fade_len = (int32_t)g_sas_grain;
        for (uint32_t i = 0; i < n && s->on; i++) {
            int32_t x = s->prev + (((s->cur - s->prev) * (int32_t)s->frac) >> 12);
            if (s->has_env) {
                env_step(s);
                if (!s->on) break;
                x = (x * (s->height >> 15)) >> 15;
            } else if (s->key_off) {
                if (s->fade <= 0) { s->on = 0; break; }
                x = x * s->fade / fade_len;
                s->fade--;
            }
            acc[i][0] += (x * s->vl) >> 12;
            acc[i][1] += (x * s->vr) >> 12;
            s->frac += (uint32_t)s->pitch;
            while (s->frac >= 0x1000u && s->on) {
                s->frac -= 0x1000u;
                s->prev = s->cur;
                s->cur = sas_next(s);
            }
        }
    }
}

uint32_t ps2_sas_core_calls;
static int32_t g_sas_acc[SAS_GRAIN_MAX][2];

static inline int16_t sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }

/* __sceSasCore(core, out): no pacing, on a PSP it returns once the grain is mixed and
 * the sound thread (0x7CF1C) paces itself on its blocking sceAudio output. */
static void hle_SasCore(void) {
    if (!sas_core_ok()) return;
    const uint32_t bytes = g_sas_grain * (g_sas_mode ? 8u : 4u);
    int16_t *out = (int16_t *)psp_mem_ptr(psp_arg(1), bytes);
    if (!out) { psp_ret(SAS_ERR_ADDRESS); return; }
    memset(out, 0, bytes);
    ps2_sas_core_calls++;
    const uint32_t n = g_sas_grain < SAS_GRAIN_MAX ? g_sas_grain : SAS_GRAIN_MAX;
    memset(g_sas_acc, 0, n * sizeof g_sas_acc[0]);
    sas_mix(g_sas_acc, n);
    if (!g_sas_mode)
        for (uint32_t i = 0; i < n; i++) { out[2 * i] = sat16(g_sas_acc[i][0]); out[2 * i + 1] = sat16(g_sas_acc[i][1]); }
    psp_ret(0);
}

/* __sceSasCoreWithMix(core, inout, l, r): the caller's PCM at volume l/r (0..0x1000), plus the voices. */
static void hle_SasCoreWithMix(void) {
    if (!sas_core_ok()) return;
    const uint32_t n = g_sas_grain < SAS_GRAIN_MAX ? g_sas_grain : SAS_GRAIN_MAX;
    int16_t *io = (int16_t *)psp_mem_ptr(psp_arg(1), n * 4u);
    if (!io) { psp_ret(SAS_ERR_ADDRESS); return; }
    const int32_t lv = (int32_t)psp_arg(2), rv = (int32_t)psp_arg(3);
    for (uint32_t i = 0; i < n; i++) {
        g_sas_acc[i][0] = (io[2 * i] * lv) >> 12;
        g_sas_acc[i][1] = (io[2 * i + 1] * rv) >> 12;
    }
    sas_mix(g_sas_acc, n);
    for (uint32_t i = 0; i < n; i++) { io[2 * i] = sat16(g_sas_acc[i][0]); io[2 * i + 1] = sat16(g_sas_acc[i][1]); }
    psp_ret(0);
}

/* Bit set = voice not sounding. Live, not a snapshot of the last core: the game must
 * see a voice it just keyed on as taken. */
static void hle_SasGetEndFlag(void) {
    if (!sas_core_ok()) return;
    uint32_t m = 0;
    for (int i = 0; i < SAS_VOICES; i++) if (!g_sv[i].on) m |= 1u << i;
    psp_ret(m);
}

uint32_t ps2_sas_endmask(void) {
    uint32_t m = 0;
    for (int i = 0; i < SAS_VOICES; i++) if (!g_sv[i].on) m |= 1u << i;
    return m;
}

static void hle_SasGetEnvelopeHeight(void) {
    const int v = sas_voice_arg();
    if (v >= 0) psp_ret(g_sv[v].on ? (uint32_t)g_sv[v].height : 0u);
}

static void hle_SasGetAllEnvelopeHeights(void) {
    if (!sas_core_ok()) return;
    for (uint32_t i = 0; i < SAS_VOICES; i++)
        psp_write32(psp_arg(1) + i * 4u, g_sv[i].on ? (uint32_t)g_sv[i].height : 0u);
    psp_ret(0);
}

static void hle_SasGetPauseFlag(void) {
    if (!sas_core_ok()) return;
    uint32_t m = 0;
    for (int i = 0; i < SAS_VOICES; i++) if (g_sv[i].paused) m |= 1u << i;
    psp_ret(m);
}

/* (core, voice mask, pause) */
static void hle_SasSetPause(void) {
    if (!sas_core_ok()) return;
    for (int i = 0; i < SAS_VOICES; i++)
        if (psp_arg(1) & (1u << i)) g_sv[i].paused = psp_arg(2) != 0;
    psp_ret(0);
}

static void hle_SasSetGrain(void) {
    if (!sas_core_ok()) return;
    g_sas_grain = psp_arg(1);
    psp_ret(0);
}
static void hle_SasGetGrain(void) { if (sas_core_ok()) psp_ret(g_sas_grain); }

static void hle_SasSetOutputmode(void) {
    if (!sas_core_ok()) return;
    g_sas_mode = psp_arg(1);
    psp_ret(0);
}
static void hle_SasGetOutputmode(void) { if (sas_core_ok()) psp_ret(g_sas_mode); }

/* Accepted and ignored: effects. */
static void hle_SasCoreOk(void)  { if (sas_core_ok()) psp_ret(0); }

/* ---- install -------------------------------------------------------------- */
void ps2_audio_install(void) {
    g_last = read_count();
    g_base = g_last;
    AddIntcHandler(2, clock_handler, 0);
    EnableIntc(2);

    psp_hle_register(0x092968F4, "Kernel_Library", "sceKernelCpuSuspendIntr", hle_SuspendIntr);
    psp_hle_register(0x5F10D406, "Kernel_Library", "sceKernelCpuResumeIntr",  hle_ResumeIntr);

    /* Time */
    psp_hle_register(0xCEADEB47, "ThreadManForUser", "sceKernelDelayThread",       hle_DelayThread);
    psp_hle_register(0x68DA9E36, "ThreadManForUser", "sceKernelDelayThreadCB",     hle_DelayThread);
    psp_hle_register(0x82BC5777, "ThreadManForUser", "sceKernelGetSystemTimeWide", hle_GetSystemTimeWide);
    psp_hle_register(0xDB738F35, "ThreadManForUser", "sceKernelGetSystemTime",     hle_GetSystemTime);
    psp_hle_register(0x369ED59D, "ThreadManForUser", "sceKernelGetSystemTimeLow",  hle_GetSystemTimeLow);
    psp_hle_register(0xBA6B92E2, "ThreadManForUser", "sceKernelSysClock2USec",     hle_SysClock2USec);
    psp_hle_register(0x91E4F6A7, "UtilsForUser", "sceKernelLibcClock",        hle_LibcClock);
    psp_hle_register(0x27CC57F0, "UtilsForUser", "sceKernelLibcTime",         hle_LibcTime);
    psp_hle_register(0x71EC4271, "UtilsForUser", "sceKernelLibcGettimeofday", hle_LibcGettimeofday);
    psp_hle_register(0xE7C27D1B, "sceRtc", "sceRtcGetCurrentClockLocalTime",  hle_RtcLocalTime);

    /* Display */
    psp_hle_register(0x984C27E7, "sceDisplay", "sceDisplayWaitVblankStart",   hle_WaitVblank);
    psp_hle_register(0x46F186C3, "sceDisplay", "sceDisplayWaitVblankStartCB", hle_WaitVblank);
    psp_hle_register(0x9C6EAAD7, "sceDisplay", "sceDisplayGetVcount",         hle_GetVcount);
    psp_hle_register(0xDBA6C4C4, "sceDisplay", "sceDisplayGetFramePerSec",    hle_GetFramePerSec);

    /* sceAudio */
    psp_hle_register(0x136CAF51, "sceAudio", "sceAudioOutputBlocking",       hle_AudioOutputBlocking);
    psp_hle_register(0x13F592BC, "sceAudio", "sceAudioOutputPannedBlocking", hle_AudioOutputPannedBlocking);
    psp_hle_register(0xE2D56B2D, "sceAudio", "sceAudioOutputPanned",         hle_AudioOutputPanned);
    psp_hle_register(0x5EC81C55, "sceAudio", "sceAudioChReserve",            hle_AudioChReserve);
    psp_hle_register(0x6FC46853, "sceAudio", "sceAudioChRelease",            hle_AudioChRelease);
    psp_hle_register(0xCB2E439E, "sceAudio", "sceAudioSetChannelDataLen",    hle_AudioSetChannelDataLen);
    psp_hle_register(0xB011922F, "sceAudio", "sceAudioGetChannelRestLength", hle_AudioGetChannelRestLength);
    psp_hle_register(0x95FD0C2D, "sceAudio", "sceAudioChangeChannelConfig",  hle_AudioChangeChannelConfig);
    psp_hle_register(0xB7E1D8E7, "sceAudio", "sceAudioChangeChannelVolume",  hle_AudioChOk);

    /* sceAtrac3plus */
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

    /* sceSasCore: the 27 imports */
    psp_hle_register(0x42778A9F, "sceSasCore", "__sceSasInit",            hle_SasInit);
    psp_hle_register(0xA3589D81, "sceSasCore", "__sceSasCore",            hle_SasCore);
    psp_hle_register(0x50A14DFC, "sceSasCore", "__sceSasCoreWithMix",     hle_SasCoreWithMix);
    psp_hle_register(0x99944089, "sceSasCore", "__sceSasSetVoice",        hle_SasSetVoice);
    psp_hle_register(0xE1CD9561, "sceSasCore", "__sceSasSetVoicePCM",     hle_SasSetVoicePCM);
    psp_hle_register(0xB7660A23, "sceSasCore", "__sceSasSetNoise",        hle_SasSetNoise);
    psp_hle_register(0xAD84D37F, "sceSasCore", "__sceSasSetPitch",        hle_SasSetPitch);
    psp_hle_register(0x76F01ACA, "sceSasCore", "__sceSasSetKeyOn",        hle_SasSetKeyOn);
    psp_hle_register(0xA0CF2FA4, "sceSasCore", "__sceSasSetKeyOff",       hle_SasSetKeyOff);
    psp_hle_register(0x68A46B95, "sceSasCore", "__sceSasGetEndFlag",      hle_SasGetEndFlag);
    psp_hle_register(0x74AE582A, "sceSasCore", "__sceSasGetEnvelopeHeight", hle_SasGetEnvelopeHeight);
    psp_hle_register(0x07F58C24, "sceSasCore", "__sceSasGetAllEnvelopeHeights", hle_SasGetAllEnvelopeHeights);
    psp_hle_register(0x2C8E6AB3, "sceSasCore", "__sceSasGetPauseFlag",    hle_SasGetPauseFlag);
    psp_hle_register(0x787D04D5, "sceSasCore", "__sceSasSetPause",        hle_SasSetPause);
    psp_hle_register(0xD1E0A01E, "sceSasCore", "__sceSasSetGrain",        hle_SasSetGrain);
    psp_hle_register(0xBD11B7C2, "sceSasCore", "__sceSasGetGrain",        hle_SasGetGrain);
    psp_hle_register(0xE855BF76, "sceSasCore", "__sceSasSetOutputmode",   hle_SasSetOutputmode);
    psp_hle_register(0xE175EF66, "sceSasCore", "__sceSasGetOutputmode",   hle_SasGetOutputmode);
    psp_hle_register(0x440CA7D8, "sceSasCore", "__sceSasSetVolume",       hle_SasSetVolume);
    psp_hle_register(0x019B25EB, "sceSasCore", "__sceSasSetADSR",         hle_SasSetADSR);
    psp_hle_register(0x9EC3676A, "sceSasCore", "__sceSasSetADSRmode",     hle_SasSetADSRmode);
    psp_hle_register(0x5F9529F6, "sceSasCore", "__sceSasSetSL",           hle_SasSetSL);
    psp_hle_register(0xCBCD4F79, "sceSasCore", "__sceSasSetSimpleADSR",   hle_SasSetSimpleADSR);
    psp_hle_register(0x33D4AB37, "sceSasCore", "__sceSasRevType",         hle_SasCoreOk);
    psp_hle_register(0x267A6DD2, "sceSasCore", "__sceSasRevParam",        hle_SasCoreOk);
    psp_hle_register(0xD5A229C9, "sceSasCore", "__sceSasRevEVOL",         hle_SasCoreOk);
    psp_hle_register(0xF983B186, "sceSasCore", "__sceSasRevVON",          hle_SasCoreOk);
}
