/* ps2_snd.c -- the PSP audio channels, mixed and sent to the SPU2 through audsrv.
 *
 * Every sceAudio output already has a start time (ps2_audio.c: the channel's queue,
 * in EE cycles). Its PCM is added into one timeline at that sample, so channels that
 * the game paces independently (sas effects, ATRAC music) stay in step without
 * talking to each other. What is older than now - SND_LAG is final and goes to
 * audsrv, which plays it on the SPU2 (44.1 kHz s16 stereo; audsrv resamples to 48k).
 *
 * Sending is done by an EE kernel thread (snd_pump), woken at every vblank and above
 * the main thread's priority, so it runs even while the game is in a long stretch that
 * never yields (loading between screens). It must: audsrv feeds the SPU2 from two
 * halves that the SPU2 keeps looping, so a starved queue repeats its last block ("4
 * but but but..." over a screen change). With the pump, a stalled game gives silence.
 * The pump thread is the only one that talks to audsrv; the guest threads (main EE
 * thread) only mix, and the pump skips a tick that lands in the middle of a mix.
 *
 * The lag exists because the scheduler is cooperative: an output call that is due
 * now can run a game frame late (a long user_main slice). A buffer that arrives after
 * its part was sent loses that part (counted in `late`).
 *
 * host:snddump.txt (any content) also writes the first SND_DUMP_S seconds of the mix
 * to host:snd.raw, to check the output without listening (written by the main thread:
 * ps2sdk's file I/O is not safe from two threads).
 */
#include "ps2_snd.h"
#include "runtime_min.h"
#include "ps2_hle.h"

#include <audsrv.h>
#include <fcntl.h>
#include <iopheap.h>
#include <kernel.h>
#include <loadfile.h>
#include <sifrpc.h>
#include <sbv_patches.h>
#include <string.h>
#include <unistd.h>

/* Calibration knobs. */
#define SND_CUSHION 256u      /* silence queued in audsrv at start (6 ms) */
#define SND_QHIGH   (2048 * 4) /* audsrv bytes queued above which frames are thinned out: the EE
                                  clock runs ~0.1% ahead of the SPU2, the queue would only grow */
#define SND_DUMP_S  60u
#define SND_PUMP_PRIO 16      /* EE kernel priority of the pump (main is set to 64) */

#define MIX_N 8192u           /* power of two, 186 ms: > SND_LAG + the longest output buffer */
static int32_t (*g_mix)[2];  /* MIX_N frames, in guest RAM (ps2_snd_init): the ELF has no room */
static volatile uint64_t g_sent;   /* absolute sample index of the next sample to send */
static volatile int g_state;  /* 0 not init, -1 failed, 1 ready, 2 running */
static volatile int g_mixing; /* main thread inside ps2_snd_mix: the pump waits a tick */
static volatile uint32_t g_late, g_drop, g_skip, g_busy;
static volatile int g_queued, g_qmin = 1 << 30;   /* audsrv bytes queued: last, lowest since the last report */
static int g_pump_tid = -1;

/* dump staging: the pump appends what it sent, main writes it out */
#define DUMP_N 8192u
static int16_t g_dump_ring[DUMP_N][2];
static volatile uint32_t g_dump_w, g_dump_r;
static int      g_dump_fd = -1;
static uint32_t g_dump_left;

extern unsigned char audsrv_irx[];
extern unsigned int audsrv_irx_len;
extern void *_gp;

uint64_t ps2_audio_now_smp(void);   /* ps2_audio.c */

static inline int16_t clamp16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }

/* Send what is final (older than now - lag). Pump thread only. */
static void snd_pump(uint64_t now) {
    if (g_state == 1) {                     /* first tick: the timeline starts now */
        g_sent = now > SND_LAG ? now - SND_LAG : 0;
        g_state = 2;
    }
    if (g_state != 2 || now < SND_LAG) return;
    const uint64_t upto = now - SND_LAG;
    if (upto <= g_sent) return;
    if (upto - g_sent > MIX_N) {           /* the pump itself did not run (cannot happen often) */
        memset(g_mix, 0, MIX_N * 8u);
        g_skip += (uint32_t)(upto - g_sent);
        g_sent = upto;
        return;
    }
    static int16_t out[1024 * 2] __attribute__((aligned(64)));
    while (g_sent < upto) {
        uint32_t k = (uint32_t)(upto - g_sent);
        const uint32_t at = (uint32_t)(g_sent & (MIX_N - 1u));
        if (k > 1024u) k = 1024u;
        if (k > MIX_N - at) k = MIX_N - at;
        for (uint32_t i = 0; i < k; i++) {
            out[2 * i] = clamp16(g_mix[at + i][0]);
            out[2 * i + 1] = clamp16(g_mix[at + i][1]);
        }
        memset(&g_mix[at], 0, k * 8u);
        g_sent += k;
        /* Above the target, leave out 1 frame in 256 (inaudible, unlike a dropped chunk):
         * up to 0.4% faster than real time, enough to drain the clock drift. */
        uint32_t ks = k;
        g_queued = audsrv_queued();
        if (g_queued < g_qmin) g_qmin = g_queued;
        if (g_queued > SND_QHIGH && k > 16u) {
            const uint32_t cut = 1u + k / 256u;
            ks = k - cut;
            g_drop += cut;
        }
        const int bytes = (int)(ks * 4u);
        if (audsrv_available() >= bytes) audsrv_play_audio((const char *)out, bytes);
        else g_drop += ks;                  /* audsrv full: only after a long stall */
        if (g_dump_fd >= 0)
            for (uint32_t i = 0; i < k && g_dump_w - g_dump_r < DUMP_N; i++, g_dump_w++) {
                g_dump_ring[g_dump_w % DUMP_N][0] = out[2 * i];
                g_dump_ring[g_dump_w % DUMP_N][1] = out[2 * i + 1];
            }
    }
}

static void pump_thread(void *arg) {
    (void)arg;
    for (;;) {
        SleepThread();                      /* woken by ps2_snd_vblank_isr */
        if (g_mixing) { g_busy++; continue; }
        snd_pump(ps2_audio_now_smp());
    }
}

/* From the vblank interrupt handler. */
void ps2_snd_vblank_isr(void) {
    if (g_pump_tid >= 0) iWakeupThread(g_pump_tid);
}

/* At boot, from main (before the guest threads). */
void ps2_snd_init(void) {
    g_state = -1;
    {
        const uint32_t uid = ps2_sysmem_alloc(MIX_N * 8u, 1u, 0);
        const uint32_t g = uid ? ps2_sysmem_head(uid) : 0;
        g_mix = g ? (int32_t (*)[2])psp_mem_ptr(g, MIX_N * 8u) : NULL;
        if (!g_mix) { psp_printf("[snd] no guest RAM for the mix\n"); return; }
        memset(g_mix, 0, MIX_N * 8u);
    }
    SifInitRpc(0);
    SifInitIopHeap();          /* SifExecModuleBuffer copies the module into IOP memory */
    SifLoadFileInit();
    sbv_patch_enable_lmb();    /* the BIOS LOADFILE cannot load a module from EE memory without it */
    int r = 0;
    const int a = SifLoadModule("rom0:LIBSD", 0, NULL);   /* as ps2sdk's audsrv samples */
    const int b = SifExecModuleBuffer(audsrv_irx, audsrv_irx_len, 0, NULL, &r);
    const int c = (a >= 0 && b >= 0) ? audsrv_init() : -1;
    psp_printf("[snd] libsd=%d audsrv=%d/%d init=%d\n", a, b, r, c);
    if (c != 0) return;
    audsrv_fmt_t fmt = { .freq = 44100, .bits = 16, .channels = 2 };
    if (audsrv_set_format(&fmt) != 0) { psp_printf("[snd] set_format: %s\n", audsrv_get_error_string()); return; }
    audsrv_set_volume(100);
    static int16_t zero[SND_CUSHION * 2];
    audsrv_play_audio((const char *)zero, sizeof zero);

    const int fd = open("host:snddump.txt", O_RDONLY);
    if (fd >= 0) {
        close(fd);
        g_dump_fd = open("host:snd.raw", O_WRONLY | O_CREAT | O_TRUNC);
        g_dump_left = SND_DUMP_S * 44100u;
        psp_printf("[snd] dumping %us of the mix to host:snd.raw (fd=%d)\n", SND_DUMP_S, g_dump_fd);
    }

    static uint8_t stack[16 * 1024] __attribute__((aligned(16)));
    ee_thread_t th;
    memset(&th, 0, sizeof th);
    th.func = (void *)pump_thread;
    th.stack = stack;
    th.stack_size = sizeof stack;
    th.gp_reg = &_gp;
    th.initial_priority = SND_PUMP_PRIO;
    ChangeThreadPriority(GetThreadId(), 64);
    g_pump_tid = CreateThread(&th);
    if (g_pump_tid < 0 || StartThread(g_pump_tid, NULL) < 0) {
        psp_printf("[snd] pump thread failed (%d)\n", g_pump_tid);
        g_pump_tid = -1;
        return;
    }
    g_state = 1;
    psp_printf("[snd] pump thread %d\n", g_pump_tid);
}

/* Main thread (guest threads). */
void ps2_snd_mix(uint64_t at, const int16_t *pcm, uint32_t n, int stereo,
                 int32_t lvol, int32_t rvol, uint64_t now) {
    (void)now;
    if (g_state != 2 || !pcm) return;
    g_mixing = 1;
    const uint64_t sent = g_sent;
    for (uint32_t i = 0; i < n; i++) {
        const uint64_t s = at + i;
        if (s < sent) { g_late++; continue; }
        if (s >= sent + MIX_N) { g_drop += n - i; break; }
        const int32_t l = stereo ? pcm[2 * i] : pcm[i];
        const int32_t r = stereo ? pcm[2 * i + 1] : pcm[i];
        int32_t *m = g_mix[s & (MIX_N - 1u)];
        m[0] += (l * lvol) >> 15;
        m[1] += (r * rvol) >> 15;
    }
    g_mixing = 0;
}

/* Main thread, once per frame: the dump file and a status line every 10 s. */
void ps2_snd_report(uint64_t now) {
    if (g_dump_fd >= 0) {
        while (g_dump_r != g_dump_w && g_dump_left) {
            uint32_t k = g_dump_w - g_dump_r;
            const uint32_t at = g_dump_r % DUMP_N;
            if (k > DUMP_N - at) k = DUMP_N - at;
            if (k > g_dump_left) k = g_dump_left;
            write(g_dump_fd, g_dump_ring[at], k * 4u);
            g_dump_r += k;
            g_dump_left -= k;
        }
        if (!g_dump_left) { close(g_dump_fd); g_dump_fd = -1; psp_printf("[snd] dump done\n"); }
    }
    static uint64_t next;
    if (now >= next) {
        next = now + 10u * 44100u;
        psp_printf("[snd] t=%us queued=%d min=%d late=%u drop=%u skip=%u busy=%u\n", (uint32_t)(now / 44100u),
                   g_queued, g_qmin, g_late, g_drop, g_skip, g_busy);
        g_qmin = 1 << 30;
    }
}
