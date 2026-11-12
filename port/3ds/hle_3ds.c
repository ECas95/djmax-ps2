/* ps2_hle.c -- the firmware calls the game needs to get past module_start.
 *
 * Everything here is a real implementation where it matters (partition memory,
 * thread creation and the blocking calls that drive the scheduler) and a
 * counted stub elsewhere. The numbers are the real NIDs: the import thunks in
 * the corpus dispatch by SHA-1-derived NID, so the table is the only way a
 * firmware call reaches code.
 */
/* hle_3ds.c -- firmware calls, ported from port/ps2/ps2_hle.c.
 *
 * The semantics are the PS2 port's (measured against the game there); only the
 * platform services differ: guest memory is the 3DS runtime's, time comes from
 * svcGetSystemTick, the pad comes from libctru hid, and the file system is
 * sdmc:. */
#include "hle_3ds.h"
#include "sched_3ds.h"
#include "runtime_3ds.h"
#include "ps2_compat.h"
#include "ge_3ds.h"
#include "video_3ds.h"
#include "audio_3ds.h"

#include <3ds.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
uint32_t ps2_vpl_base(uint32_t uid);
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

/* ======================================================================
 * SysMem: partition allocator over the 8 MB guest RAM window.
 * ==================================================================== */
#define SMEM_MAX      256
#define SMEM_GRAN     0x100u
#define HEAP_LO       0x08020000u
#define HEAP_HI       0x091F0000u   /* heap ends here; above is the stack area (ps2_stack_alloc) */

typedef struct {
    int      used;
    uint32_t uid;
    uint32_t addr;
    uint32_t size;
} smem_blk;

static void log_wait(const char *what, uint32_t uid);
static void log_sig(const char *what, uint32_t uid, uint32_t val);

uint32_t ps2_song_last = 0;   /* base game: no song overlay (port/ps2/ps2_songs.c) */

static smem_blk g_smem[SMEM_MAX];
static uint32_t g_smem_uid = 0x00010001u;

void ps2_sysmem_init(void) {
    memset(g_smem, 0, sizeof g_smem);
    g_smem_uid = 0x00010000u;
}

static int smem_overlaps(uint32_t addr, uint32_t size) {
    for (int i = 0; i < SMEM_MAX; i++) {
        if (!g_smem[i].used) continue;
        uint32_t b = g_smem[i].addr, e = b + g_smem[i].size;
        if (addr < e && b < addr + size) return 1;
    }
    return 0;
}

static uint32_t place_low(uint32_t size, uint32_t align) {
    uint32_t a = (HEAP_LO + align - 1) & ~(align - 1);
    while (a + size <= HEAP_HI) {
        if (!smem_overlaps(a, size)) return a;
        uint32_t next = HEAP_HI;
        for (int i = 0; i < SMEM_MAX; i++) {
            if (!g_smem[i].used) continue;
            uint32_t b = g_smem[i].addr, e = b + g_smem[i].size;
            if (a < e && b < a + size && e < next) next = e;
        }
        a = (next + align - 1) & ~(align - 1);
    }
    return 0;
}

static uint32_t place_high(uint32_t size, uint32_t align) {
    if (size > HEAP_HI - HEAP_LO) return 0;
    uint32_t a = (HEAP_HI - size) & ~(align - 1);
    while (a >= HEAP_LO) {
        if (!smem_overlaps(a, size)) return a;
        uint32_t prev = HEAP_LO;
        for (int i = 0; i < SMEM_MAX; i++) {
            if (!g_smem[i].used) continue;
            uint32_t b = g_smem[i].addr, e = b + g_smem[i].size;
            if (a < e && b < a + size && b > prev) prev = b;
        }
        if (prev <= HEAP_LO || prev < size) return 0;
        a = (prev - size) & ~(align - 1);
    }
    return 0;
}

uint32_t ps2_sysmem_alloc(uint32_t size, uint32_t type, uint32_t addr_hint) {
    if (size == 0) size = SMEM_GRAN;
    size = (size + SMEM_GRAN - 1) & ~(SMEM_GRAN - 1);

    uint32_t align = SMEM_GRAN;
    if (type == 3u || type == 4u) {            /* LowAligned / HighAligned */
        align = addr_hint ? addr_hint : SMEM_GRAN;
        if (align < SMEM_GRAN) align = SMEM_GRAN;
        if (align & (align - 1)) return 0;
    }

    uint32_t addr;
    if (type == 2u) {                          /* PSP_SMEM_Addr */
        addr = addr_hint & ~(align - 1);
        if (addr < HEAP_LO || addr + size > HEAP_HI || smem_overlaps(addr, size))
            return 0;
    } else if (type == 1u || type == 4u) {
        addr = place_high(size, align);
    } else {
        addr = place_low(size, align);
    }
    if (!addr) return 0;

    for (int i = 0; i < SMEM_MAX; i++) {
        if (g_smem[i].used) continue;
        g_smem[i].used = 1;
        g_smem[i].uid = g_smem_uid++;
        g_smem[i].addr = addr;
        g_smem[i].size = size;
        return g_smem[i].uid;
    }
    return 0;
}

static smem_blk *smem_find(uint32_t uid) {
    for (int i = 0; i < SMEM_MAX; i++)
        if (g_smem[i].used && g_smem[i].uid == uid) return &g_smem[i];
    return NULL;
}

int ps2_sysmem_free_block(uint32_t uid) {
    smem_blk *b = smem_find(uid);
    if (!b) return -1;
    b->used = 0;
    return 0;
}

uint32_t ps2_sysmem_head(uint32_t uid) {
    smem_blk *b = smem_find(uid);
    return b ? b->addr : 0;
}

/* Thread stacks live in a reserved 1 MB region at the top of guest RAM
 * (0x09100000..0x091F0000), below module_start's own stack at 0x091FFF00.
 * The bump-down pointer keeps them out of every partition/VPL the game asks
 * for, so the map is: [heap blocks][stack area][module_start stack]. */
static uint32_t g_stack_top = 0x092F0000u;

uint32_t ps2_stack_alloc(uint32_t size) {
    if (!size) size = 0x40000u;
    size = (size + 15u) & ~15u;
    if (size > g_stack_top || g_stack_top - size < 0x091F0000u) return 0;
    g_stack_top -= size;
    return g_stack_top;
}

uint32_t ps2_sysmem_total_free(void) {
    uint32_t used = 0;
    for (int i = 0; i < SMEM_MAX; i++) if (g_smem[i].used) used += g_smem[i].size;
    return (HEAP_HI - HEAP_LO) - used;
}

/* Largest contiguous gap between the blocks already handed out. The game uses
 * this (and TotalFreeMemSize) to size its heap, so it has to agree with what
 * AllocPartitionMemory can actually place. */
uint32_t ps2_sysmem_max_free(void) {
    uint32_t a[SMEM_MAX];
    int n = 0;
    for (int i = 0; i < SMEM_MAX; i++)
        if (g_smem[i].used) a[n++] = g_smem[i].addr;

    /* insertion sort: n is a few dozen at most */
    for (int i = 1; i < n; i++) {
        uint32_t v = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; }
        a[j + 1] = v;
    }

    uint32_t cursor = HEAP_LO, best = 0;
    for (int i = 0; i < n; i++) {
        if (a[i] > cursor) {
            uint32_t gap = a[i] - cursor;
            if (gap > best) best = gap;
        }
        /* skip the block occupying [a[i], a[i]+size) */
        for (int k = 0; k < SMEM_MAX; k++)
            if (g_smem[k].used && g_smem[k].addr == a[i]) {
                uint32_t end = a[i] + g_smem[k].size;
                if (end > cursor) cursor = end;
                break;
            }
    }
    if (HEAP_HI > cursor) {
        uint32_t gap = HEAP_HI - cursor;
        if (gap > best) best = gap;
    }
    return best;
}

/* ---- SysMem NIDs ------------------------------------------------------ */
static void hle_AllocPartitionMemory(void) {
    /* (partitionid, name, type, size, addr) */
    uint32_t type = psp_arg(2), size = psp_arg(3), want = psp_arg(4);
    uint32_t uid = ps2_sysmem_alloc(size, type, want);
    if (uid) {
        void *p = psp_mem_ptr(ps2_sysmem_head(uid), size);
        if (p) memset(p, 0, size);              /* firmware hands out clean memory */
    }
    psp_printf("[map] AllocPartition size=%u -> uid=0x%X base=0x%08X end=0x%08X\n",
               size, uid, uid ? ps2_sysmem_head(uid) : 0,
               uid ? ps2_sysmem_head(uid) + size : 0);
    psp_ret(uid ? uid : 0x80020190u);          /* SCE_KERNEL_ERROR_NO_MEMORY */
}
static void hle_FreePartitionMemory(void) { psp_ret(ps2_sysmem_free_block(psp_arg(0)) == 0 ? 0 : 0x80020001u); }
static void hle_GetBlockHeadAddr(void) {
    const uint32_t uid = psp_arg(0);
    uint32_t a = ps2_vpl_base(uid);
    if (!a) a = ps2_sysmem_head(uid);
    { static uint32_t l; if (l < 6) { l++; psp_printf("[hle] GetBlockHeadAddr uid=0x%X -> 0x%08X\n", uid, a); } }
    psp_ret(a ? a : 0x80020001u);
}
static uint32_t g_mem_logged;
static void hle_MaxFreeMemSize(void)   {
    const uint32_t v = ps2_sysmem_max_free();
    if (!g_mem_logged) { g_mem_logged = 1; psp_printf("[hle] max free = %u (total %u)\n", v, ps2_sysmem_total_free()); }
    psp_ret(v);
}
static void hle_TotalFreeMemSize(void) {
    static uint32_t logged;
    const uint32_t v = ps2_sysmem_total_free();
    if (logged < 3) { logged++; psp_printf("[hle] total free = %u, max %u\n", v, ps2_sysmem_max_free()); }
    psp_ret(v);
}
static void hle_Ok0(void) { psp_ret(0); }

/* sceKernelPrintf: the format string is all that can be recovered reliably;
 * print it verbatim so a guest error message reaches the TTY. */
static void hle_Printf(void) {
    char buf[192];
    psp_str(psp_arg(0), buf, sizeof buf);
    psp_tty_write("[guest] ");
    psp_tty_write(buf);
    psp_tty_write("\n");
    psp_ret(0);
}

/* Interrupt masking: hand back the previous state, as the hardware does. */
static uint32_t g_intr_state;
static void hle_SuspendIntr(void) {
    const uint32_t prev = g_intr_state;
    g_intr_state = 1;
    psp_ret(prev);
}
static void hle_ResumeIntr(void) {
    const uint32_t prev = g_intr_state;
    g_intr_state = psp_arg(0) & 1u;
    psp_ret(prev);
}

/* ======================================================================
 * ThreadMan: create/start/exit/delay/sleep, plus sems and event flags.
 * ==================================================================== */
static void hle_CreateThread(void) {
    char name[32];
    psp_str(psp_arg(0), name, sizeof name);
    uint32_t uid = 0;
    if (ps2_sched_create(psp_arg(1), name, psp_arg(2), psp_arg(3), 0, 0, &uid) != 0) {
        psp_ret(0x80020190u);
        return;
    }
    psp_ret(uid);
}

static void hle_StartThread(void) {
    if (ps2_sched_start(psp_arg(0), psp_arg(1), psp_arg(2)) != 0) {
        psp_ret(0x800201A5u);                  /* ILLEGAL_THID */
        return;
    }
    psp_ret(0);
    ps2_sched_preempt();                        /* a more urgent thread runs at once */
}

/* sceKernelChangeThreadPriority(uid or 0 = self, prio) */
static void hle_ChangeThreadPriority(void) {
    if (ps2_sched_set_prio(psp_arg(0), psp_arg(1)) != 0) { psp_ret(0x800201A5u); return; }
    psp_ret(0);
    ps2_sched_preempt();
}

static void hle_ExitThread(void) { ps2_sched_exit(psp_arg(0)); }
static void hle_DeleteThread(void) { psp_ret(ps2_sched_delete(psp_arg(0)) == 0 ? 0 : 0x800201A5u); }

static void hle_WaitThreadEnd(void) {
    psp_ret(ps2_sched_wait_dead(psp_arg(0)) == 0 ? 0 : 0x800201A5u);
}

static void hle_DelayThread(void) {
    log_wait("delay", 0);
    /* Honrar los µs pedidos contra el reloj real (modelo PS2: wait_until). */
    uint32_t us = psp_arg(0);
    {   static uint32_t dl;
        if (dl < 24u) { dl++;
            psp_printf("[adiag] delay th=0x%X us=%u\n", ps2_sched_current_uid(), us); } }
    if (us > 250000u) us = 250000u;
    /* OJO: también los delays de 100 µs del hilo SAS van por wait_until: el
     * deadline registrado es lo que hace que los preempts lo prioricen. */
    ps2_audio_wait_until(ps2_time_us() + us);
    psp_ret(0);
}

static void hle_SleepThread(void) {
    log_wait("sleep", 0);
    /* psprecomp caps a sleep at one tick: a poll loop that nothing ever wakes
     * must still let the other threads run, or the run stops with no evidence. */
    ps2_sched_yield();
    psp_ret(0);
}

static void hle_GetThreadId(void) { psp_ret(ps2_sched_current_uid()); }

static void hle_GetThreadExitStatus(void) {
    uint32_t out = psp_arg(1);
    (void)psp_arg(0);
    if (out) psp_write32(out, 0);
    psp_ret(0);
}

static uint32_t g_ticks;
/* Real microseconds from COP0 Count (294.912 MHz). Extended on every read, so it holds
 * as long as the guest reads time at least every 14 s.
 * ponytail: superseded by ps2_audio.c when that lands. */
static uint64_t time_us(void) {
    /* ARM11 system tick, 268.123480 MHz. Same 64-bit extension idea as the EE. */
    static uint32_t last;
    static uint64_t total;
    const uint32_t now = (uint32_t)svcGetSystemTick();
    total += now - last;
    last = now;
    return total * 1000000ull / 268123480ull;
}
static void hle_GetSystemTime(void) {
    const uint64_t t = time_us();
    const uint32_t out = psp_arg(0);
    if (out) { psp_write32(out, (uint32_t)t); psp_write32(out + 4u, (uint32_t)(t >> 32)); }
    psp_ret(0);
}
static void hle_GetSystemTimeWide(void) {
    const uint64_t t = time_us();
    psp_ret((uint32_t)t);
    psp_cpu.r[PSP_REG_V1] = (uint32_t)(t >> 32);
}
static void hle_GetSystemTimeLow(void) { psp_ret((uint32_t)time_us()); }
/* sceKernelSysClock2USec(clock*, sec*, usec*) */
static void hle_SysClock2USec(void) {
    const uint32_t c = psp_arg(0);
    const uint64_t t = c ? ((uint64_t)psp_read32(c + 4u) << 32) | psp_read32(c) : 0;
    if (psp_arg(1)) psp_write32(psp_arg(1), (uint32_t)(t / 1000000u));
    if (psp_arg(2)) psp_write32(psp_arg(2), (uint32_t)(t % 1000000u));
    psp_ret(0);
}

/* Semaphores: a real counter, and Wait blocks by yielding. */
#define SEMA_MAX 32
static struct { int used; uint32_t uid; int count, max; } g_sema[SEMA_MAX];
static uint32_t g_sema_uid = 0x00020000u;

static void hle_CreateSema(void) {
    for (int i = 0; i < SEMA_MAX; i++) {
        if (g_sema[i].used) continue;
        g_sema[i].used = 1;
        g_sema[i].uid = g_sema_uid++;
        g_sema[i].count = (int)psp_arg(2);
        g_sema[i].max = (int)psp_arg(3);
        psp_ret(g_sema[i].uid);
        return;
    }
    psp_ret(0x80020190u);
}
static void hle_DeleteSema(void) {
    for (int i = 0; i < SEMA_MAX; i++)
        if (g_sema[i].used && g_sema[i].uid == psp_arg(0)) { g_sema[i].used = 0; psp_ret(0); return; }
    psp_ret(0x80020001u);
}
static void hle_SignalSema(void) {
    log_sig("signal_sema", psp_arg(0), 1);
    for (int i = 0; i < SEMA_MAX; i++)
        if (g_sema[i].used && g_sema[i].uid == psp_arg(0)) {
            if (g_sema[i].count < g_sema[i].max) g_sema[i].count++;
            psp_ret(0);
            return;
        }
    psp_ret(0x80020001u);
}
static void hle_WaitSema(void) {
    log_wait("wait_sema", psp_arg(0));
    for (;;) {
        for (int i = 0; i < SEMA_MAX; i++)
            if (g_sema[i].used && g_sema[i].uid == psp_arg(0)) {
                if (g_sema[i].count > 0) { g_sema[i].count--; psp_ret(0); return; }
                goto wait;
            }
        psp_ret(0x80020001u);
        return;
    wait:
        ps2_sched_yield();
    }
}
static void hle_PollSema(void) {
    for (int i = 0; i < SEMA_MAX; i++)
        if (g_sema[i].used && g_sema[i].uid == psp_arg(0)) {
            if (g_sema[i].count > 0) { g_sema[i].count--; psp_ret(0); }
            else psp_ret(0x800201A7u);         /* SCE_KERNEL_ERROR_WAIT_TIMEOUT-ish */
            return;
        }
    psp_ret(0x80020001u);
}

/* Event flags: creation and setting are enough to unblock init code. */
#define EF_MAX 32
static struct { int used; uint32_t uid; uint32_t bits; } g_ef[EF_MAX];
static uint32_t g_ef_uid = 0x00030000u;

static void hle_CreateEventFlag(void) {
    for (int i = 0; i < EF_MAX; i++) {
        if (g_ef[i].used) continue;
        g_ef[i].used = 1;
        g_ef[i].uid = g_ef_uid++;
        g_ef[i].bits = psp_arg(2);
        psp_ret(g_ef[i].uid);
        return;
    }
    psp_ret(0x80020190u);
}
static void hle_SetEventFlag(void) {
    const uint32_t uid = psp_arg(0), bits = psp_arg(1);
    log_sig("set_ef", uid, bits);
    for (int i = 0; i < EF_MAX; i++)
        if (g_ef[i].used && g_ef[i].uid == uid) {
            g_ef[i].bits |= bits;
            ps2_sched_wake_ef(uid);
            psp_ret(0);
            return;
        }
    psp_ret(0);
}

/* 1 = condition met (out written, clear applied), 0 = not met, -1 = bad uid.
 * PSP modes: bit0 set = OR, clear = AND; 0x10 clears all, 0x20 clears the pattern. */
static int ef_try(uint32_t uid, uint32_t want, uint32_t mode, uint32_t out) {
    for (int i = 0; i < EF_MAX; i++) {
        if (!g_ef[i].used || g_ef[i].uid != uid) continue;
        const uint32_t cur = g_ef[i].bits;
        if (!((mode & 1u) ? (cur & want) != 0 : (cur & want) == want)) return 0;
        if (out) psp_write32(out, cur);
        if (mode & 0x10u)      g_ef[i].bits = 0;
        else if (mode & 0x20u) g_ef[i].bits &= ~want;
        return 1;
    }
    return -1;
}

static void hle_WaitEventFlag(void) {
    const uint32_t uid = psp_arg(0), want = psp_arg(1);
    log_wait("wait_ef", uid);
    const uint32_t mode = psp_arg(2), out = psp_arg(3);
    for (;;) {
        const int r = ef_try(uid, want, mode, out);
        if (r < 0) { psp_ret(0x80020198u); return; }   /* UNKNOWN_EVFID */
        if (r > 0) { psp_ret(0); return; }
        ps2_sched_block_ef(uid);
    }
}

static void hle_PollEventFlag(void) {
    const int r = ef_try(psp_arg(0), psp_arg(1), psp_arg(2), psp_arg(3));
    psp_ret(r < 0 ? 0x80020198u : r == 0 ? 0x800201AFu : 0);   /* EVF_COND */
}

/* sceKernelClearEventFlag(uid, bits): bits are the mask to KEEP. */
static void hle_ClearEventFlag(void) {
    for (int i = 0; i < EF_MAX; i++)
        if (g_ef[i].used && g_ef[i].uid == psp_arg(0)) {
            g_ef[i].bits &= psp_arg(1);
            psp_ret(0);
            return;
        }
    psp_ret(0x80020198u);
}

/* sceKernelReferEventFlagStatus(uid, SceKernelEventFlagInfo *): size(+0, the caller's),
 * name[32], attr(+36), initPattern(+40), currentPattern(+44), numWaitThreads(+48).
 * The movie player polls it (0x8ABA0); unimplemented, it left the struct as it was and
 * the player waited forever after 8 s of the opening movie. */
static void hle_ReferEventFlagStatus(void) {
    const uint32_t info = psp_arg(1);
    for (int i = 0; i < EF_MAX; i++)
        if (g_ef[i].used && g_ef[i].uid == psp_arg(0)) {
            if (info && psp_mem_ptr(info, 52)) {
                psp_write32(info + 36, 0);
                psp_write32(info + 40, 0);
                psp_write32(info + 44, g_ef[i].bits);
                psp_write32(info + 48, 0);
            }
            psp_ret(0);
            return;
        }
    psp_ret(0x80020198u);
}

static void hle_DeleteEventFlag(void) {
    for (int i = 0; i < EF_MAX; i++)
        if (g_ef[i].used && g_ef[i].uid == psp_arg(0)) { g_ef[i].used = 0; break; }
    psp_ret(0);
}

/* VPL: allocate guest RAM and hand the address back through the out pointer. */
/* ---- VPL: real pools carved out of one sysmem reservation -----------------
 * CreateVpl reserves `size` bytes; every AllocateVpl takes a 64-byte-aligned
 * slice of that reservation (it must NOT ask sysmem for new memory, or the
 * game's idea of the pool and ours diverge and its free() aborts). A free of
 * the top-most slice rewinds the bump pointer, which is enough for the
 * allocate/free patterns init code uses.
 */
#define VPL_MAX    8
#define VPL_BLOCKS 4096   /* gameplay: ~200 samples, ~150 BGA files, in guest RAM */
#define VPL_ALIGN  16u
typedef struct {
    int      used;
    uint32_t uid;
    uint32_t block_uid;     /* sysmem uid of the reservation */
    uint32_t base;
    uint32_t size;          /* bytes requested for the pool */
    uint32_t nblk;
    struct vpl_blk { uint32_t off, size; } *blk;      /* in use, sorted by off */
    uint32_t tbl_uid;       /* sysmem uid of that table */
    char     name[16];
} vpl_pool;

static vpl_pool g_vpl[VPL_MAX];
static uint32_t g_vpl_uid = 0x00050000u;

void ps2_vpl_report(void) {
    for (uint32_t i=0; i<VPL_MAX; i++) if (g_vpl[i].used && g_vpl[i].uid==0x50000u) {
        const vpl_pool *v=&g_vpl[i]; uint32_t used=0;
        for (uint32_t j=0; j<v->nblk; j++) used+=v->blk[j].size;
        psp_printf("[vpl] gameplay used=%u free=%u total=%u blocks=%u\n", used, v->size-used, v->size, v->nblk);
    }
}

uint32_t ps2_vpl_base(uint32_t uid) {
    for (int i = 0; i < VPL_MAX; i++)
        if (g_vpl[i].used && g_vpl[i].uid == uid) return g_vpl[i].base;
    return 0;
}

static vpl_pool *vpl_find(uint32_t uid) {
    for (int i = 0; i < VPL_MAX; i++)
        if (g_vpl[i].used && g_vpl[i].uid == uid) return &g_vpl[i];
    return NULL;
}

static void hle_CreateVpl(void) {
    /* (name, partitionid, attr, size, option) */
    vpl_pool *v = NULL;
    for (int i = 0; i < VPL_MAX; i++) if (!g_vpl[i].used) { v = &g_vpl[i]; break; }
    if (!v) { psp_ret(0x80020190u); return; }

    uint32_t size = psp_arg(3);
    psp_printf("[hle] CreateVpl a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X a4=0x%08X\n",
               psp_arg(0), psp_arg(1), psp_arg(2), psp_arg(3), psp_arg(4));
    if (!size) size = 0x100;
    /* The pack manager keeps the TOCs of all.mp and all2.mp in this 160 KB pool; the songs
     * overlay (ps2_songs.c) makes them bigger. 640 KB stay free after the boot pools. */
    { extern uint32_t ps2_song_last; if (size == 0x28000u && ps2_song_last > 60u) size = 0x40000u; }
    const uint32_t blk = ps2_sysmem_alloc(size, 0, 0);
    if (!blk) { psp_printf("[hle] CreateVpl alloc(%u) failed, free=%u\n", size, ps2_sysmem_total_free()); psp_ret(0x80020190u); return; }

    memset(v, 0, sizeof *v);
    v->used = 1;
    v->uid = g_vpl_uid++;
    v->block_uid = blk;
    v->base = ps2_sysmem_head(blk);
    v->size = size;
    /* The block table lives in guest RAM: EE RAM has no room for 8 x 32 KB. */
    const uint32_t tbl = ps2_sysmem_alloc(VPL_BLOCKS * sizeof(struct vpl_blk), 0, 0);
    v->tbl_uid = tbl;
    v->blk = tbl ? (struct vpl_blk *)psp_mem_ptr(ps2_sysmem_head(tbl), VPL_BLOCKS * sizeof(struct vpl_blk)) : NULL;
    if (!v->blk) { v->used = 0; ps2_sysmem_free_block(blk); psp_ret(0x80020190u); return; }
    psp_str(psp_arg(0), v->name, sizeof v->name);
    psp_printf("[hle] CreateVpl size=%u base=0x%08X uid=0x%X free=%u\n",
               size, v->base, v->uid, ps2_sysmem_total_free());
    psp_ret(v->uid);
}

/* First fit over the blocks in use, kept sorted by offset. Freed space is reusable:
 * the game's main heap lives here and it allocates and frees on every screen. */
static uint32_t vpl_take(vpl_pool *v, uint32_t size) {
    size = (size + VPL_ALIGN - 1u) & ~(VPL_ALIGN - 1u);
    if (!size) size = VPL_ALIGN;
    if (v->nblk == VPL_BLOCKS) {
        psp_printf("[hle] VPL 0x%X: block table full (%u)\n", v->uid, v->nblk);
        return 0;
    }
    uint32_t off = 0, i = 0;
    for (; i < v->nblk; i++) {
        if (v->blk[i].off - off >= size) break;
        off = v->blk[i].off + v->blk[i].size;
    }
    if (size > v->size || off > v->size - size) {
        uint32_t used = 0;
        for (uint32_t k = 0; k < v->nblk; k++) used += v->blk[k].size;
        psp_printf("[hle] VPL 0x%X full: want %u, used %u of %u in %u blocks\n",
                   v->uid, size, used, v->size, v->nblk);
        { static int dumped;
          if (!dumped) {
            const int marker = open("host:heapdump.txt", O_RDONLY);
            if (marker >= 0) {
              close(marker); dumped = 1;
              const int out = open("host:vpl-full.bin", O_WRONLY | O_CREAT | O_TRUNC, 0666);
              if (out >= 0) { write(out, psp_mem_ptr(v->base, v->size), v->size); close(out); }
              for (uint32_t k = 0; k < v->nblk; k++)
                psp_printf("[vplblk] %08X %u\n", v->base + v->blk[k].off, v->blk[k].size);
            }
          }
        }
        return 0;
    }
    memmove(&v->blk[i + 1], &v->blk[i], (v->nblk - i) * sizeof v->blk[0]);
    v->blk[i].off = off;
    v->blk[i].size = size;
    v->nblk++;
    return v->base + off;
}

/* 0, or UNKNOWN_VPLID for an address that is not a live block of this pool. */
static uint32_t vpl_give(vpl_pool *v, uint32_t addr) {
    const uint32_t off = addr - v->base;
    for (uint32_t i = 0; i < v->nblk; i++) {
        if (v->blk[i].off != off) continue;
        v->nblk--;
        memmove(&v->blk[i], &v->blk[i + 1], (v->nblk - i) * sizeof v->blk[0]);
        return 0;
    }
    return 0x8002019Au;
}

/* sceKernelAllocateVpl / CB / Try: (uid, size, void **out, timeout*) -> 0 or error. */
static void hle_AllocateVpl(void) {
    vpl_pool *v = vpl_find(psp_arg(0));
    const uint32_t out = psp_arg(2);
    if (!v) { psp_ret(0x8002019Au); return; }        /* UNKNOWN_VPLID */
    const uint32_t a = vpl_take(v, psp_arg(1));
    /* ponytail: the plain and CB calls block on a PSP until memory frees; here they fail. */
    if (!a) { psp_ret(0x800200D9u); return; }
    if (out) psp_write32(out, a);
    psp_ret(0);
}

static void hle_FreeVpl(void) {
    vpl_pool *v = vpl_find(psp_arg(0));
    psp_ret(v ? vpl_give(v, psp_arg(1)) : 0x8002019Au);
}

static void hle_DeleteVpl(void) {
    vpl_pool *v = vpl_find(psp_arg(0));
    if (v) { ps2_sysmem_free_block(v->tbl_uid); ps2_sysmem_free_block(v->block_uid); v->used = 0; }
    psp_ret(0);
}

/* ======================================================================
 * Everything else: report plausible values, count the call.
 * ==================================================================== */
/* IoFileMgr: disc0:/X is served from host:X. PCSX2 resolves host: relative to
 * the ELF's folder, so build/ps2/out/PSP_GAME is a junction to the game tree
 * (run_pcsx2.ps1 creates it).
 * ponytail: host: only exists in the emulator; cdrom0: when building a disc.
 *   ms0:/... -> an empty file (saves do not block).
 *   anything else -> NOENT. */
#define IO_FD_MAX   16

/* ovl_fd >= 0: the file is a pack with an overlay (tools-src/songpack.py): served as
 * ovl head [0, hl) + real [r0, rs) + ovl side, all read through io_vread. */
typedef struct { int used; int host_fd; uint32_t off; uint32_t size; int ovl_fd; uint32_t hl, r0, rs, side_at; } io_fd;
static io_fd  g_fd[IO_FD_MAX];
static uint32_t g_io_open_logged, g_io_reads;
uint32_t ps2_io_total_reads;

static int io_prefix(const char *p, const char *pref) {
    while (*pref) { if (*p++ != *pref++) return 0; }
    return 1;
}

static int io_alloc(int host_fd, uint32_t size) {
    for (int fd = 3; fd < IO_FD_MAX; fd++) {
        if (!g_fd[fd].used) {
            g_fd[fd].used = 1;
            g_fd[fd].off = 0;
            g_fd[fd].size = size;
            g_fd[fd].host_fd = host_fd;
            g_fd[fd].ovl_fd = -1;
            return fd;
        }
    }
    return -1;
}

/* Returns a host fd (and the file size), or < 0.
 *
 * 3DS: the PSP disc is the SD tree under sdmc:/djmax/. disc0:/PSP_GAME/... maps
 * 1:1 to sdmc:/djmax/PSP_GAME/... (libctru's fs devoptab resolves the path).
 * The base game has no overlay, so ovl_fd stays -1 for every open. */
#define DJMX_SD_ROOT "sdmc:/djmax/"

/* Raíz del juego en la SD: acepta las variantes típicas de copia y, si no hay
 * ninguna, lista en pantalla lo que sí existe (en consola real la causa más
 * común de "no arranca" es la SD mal copiada). */
static const char *io_sd_root(void) {
    static const char *ok;
    if (ok) return ok;
    static const char *cand[] = {
        "sdmc:/djmax/", "sdmc:/DJMAX/", "sdmc:/djmax/djmax/", "sdmc:/",
    };
    for (unsigned i = 0; i < sizeof cand / sizeof cand[0]; i++) {
        char probe[96];
        snprintf(probe, sizeof probe, "%sPSP_GAME/USRDIR", cand[i]);
        struct stat st;
        if (stat(probe, &st) == 0) {
            ok = cand[i];
            psp_printf("[io] raiz del juego: %s\n", ok);
            return ok;
        }
    }
    static uint32_t miss;
    if ((miss++ % 600u) == 0u) {
        psp_printf("[io] ERROR: no encuentro PSP_GAME en la SD.\n");
        psp_printf("[io] Copia tu juego a sdmc:/djmax/PSP_GAME/...\n");
        DIR *d = opendir("sdmc:/djmax");
        const char *shown = "sdmc:/djmax";
        if (!d) { d = opendir("sdmc:/"); shown = "sdmc:/"; }
        if (d) {
            psp_printf("[io] contenido de %s:\n", shown);
            struct dirent *e;
            int n = 0;
            while ((e = readdir(d)) != NULL && n < 10) {
                psp_printf("[io]   %s\n", e->d_name);
                n++;
            }
            if (!n) psp_printf("[io]   (vacio)\n");
            closedir(d);
        } else {
            psp_printf("[io] no puedo leer la SD (sdmc sin montar, errno=%d)\n", errno);
        }
    }
    return NULL;
}

static int io_host_open(const char *p, uint32_t *size) {
    char hp[256];
    p += 6;                                 /* "disc0:" */
    while (*p == '/') p++;
    const char *root = io_sd_root();
    if (!root) return -1;
    snprintf(hp, sizeof hp, "%s%s", root, p);
    const int hf = open(hp, O_RDONLY);
    if (hf < 0) {
        static uint32_t elogs;
        if (elogs < 6) { elogs++;
            psp_printf("[io] open('%s') fallo errno=%d\n", hp, errno); }
        return hf;
    }
    *size = (uint32_t)lseek(hf, 0, SEEK_END);
    lseek(hf, 0, SEEK_SET);
    return hf;
}

/* A pack the songs overlay extends (host:PS2_DATA/ovl/<name>.ovl): the more songs work
 * (ps2_songs.c) without rewriting the 1 GB of original packs. */
static void io_ovl_attach(int fd, const char *p) {
#ifdef DJMAX_BASE_ONLY
    return;
#endif
    const char *b = strrchr(p, '/');
    b = b ? b + 1 : p;
    if (strcmp(b, "all.mp") && strcmp(b, "all2.mp")) return;
    char hp[96];
    snprintf(hp, sizeof hp, "host:PS2_DATA/ovl/%s.ovl", b);
    const int of = open(hp, O_RDONLY);
    if (of < 0) return;
    uint32_t h[5];
    if (read(of, h, sizeof h) != (int)sizeof h || h[0] != 0x314C564Fu /* "OVL1" */ || h[3] != g_fd[fd].size) {
        psp_printf("[io] overlay %s does not match the pack: ignored\n", hp);
        close(of);
        return;
    }
    io_fd *f = &g_fd[fd];
    f->ovl_fd = of;
    f->hl = h[1]; f->r0 = h[2]; f->rs = h[3]; f->side_at = 20u + h[1];
    f->size = h[1] + (h[3] - h[2]) + h[4];
    psp_printf("[io] %s: overlay, %u -> %u bytes\n", b, h[3], f->size);
}

/* n bytes at offset off of an open file (overlay or plain). */
static int io_vread(io_fd *f, uint8_t *dst, uint32_t off, uint32_t n) {
    if (f->ovl_fd < 0) {
        lseek(f->host_fd, (off_t)off, SEEK_SET);
        const int got = (int)read(f->host_fd, dst, (size_t)n);
        return got < 0 ? 0 : got;
    }
    const uint32_t real_end = f->hl + (f->rs - f->r0);
    int total = 0;
    while (n) {
        int hfd;
        uint32_t at, lim;
        if (off < f->hl)        { hfd = f->ovl_fd;  at = 20u + off;                   lim = f->hl - off; }
        else if (off < real_end) { hfd = f->host_fd; at = f->r0 + (off - f->hl);       lim = real_end - off; }
        else                     { hfd = f->ovl_fd;  at = f->side_at + (off - real_end); lim = f->size - off; }
        const uint32_t k = n < lim ? n : lim;
        lseek(hfd, (off_t)at, SEEK_SET);
        const int got = (int)read(hfd, dst, (size_t)k);
        if (got <= 0) break;
        total += got; dst += got; off += (uint32_t)got; n -= (uint32_t)got;
        if ((uint32_t)got < k) break;
    }
    return total;
}

/* Loose files of the extra songs (tools-src/songpack.py, loose.ovl): a disc0: path the disc
 * does not have is looked up by the hash of its USRDIR-relative name, lowercase (the pack
 * TOC hash). The game asks for such a path when no TOC has the file. */
#define LOOSE_MAX 2048
static struct { uint32_t hash, off, size; } g_loose[LOOSE_MAX];
static uint32_t g_loose_n;

static uint32_t name_hash(const char *s) {
    uint32_t h = (uint32_t)(uint8_t)tolower((unsigned char)s[0]) << 7, n = 0;
    for (; s[n]; n++) h = (h * 1000003u) ^ (uint8_t)tolower((unsigned char)s[n]);
    h ^= n;
    return h == 0xFFFFFFFFu ? 0xFFFFFFFEu : h;
}

static void io_loose_init(void) {
#ifdef DJMAX_BASE_ONLY
    return;
#endif
    const int fd = open("host:PS2_DATA/ovl/loose.ovl", O_RDONLY);
    if (fd < 0) return;
    uint32_t h[2];
    if (read(fd, h, 8) == 8 && h[0] == 0x3145534Cu /* "LSE1" */ && h[1] <= LOOSE_MAX &&
        read(fd, g_loose, h[1] * 12u) == (int)(h[1] * 12u))
        g_loose_n = h[1];
    close(fd);
    psp_printf("[io] loose files: %u\n", g_loose_n);
}

static int io_loose_open(const char *p) {
    static int ready;
    if (!ready) { ready = 1; io_loose_init(); }
    const char *rel = p + 6;                                  /* "disc0:" */
    while (*rel == '/') rel++;
    if (!strncasecmp(rel, "PSP_GAME/USRDIR/", 16)) rel += 16;
    const uint32_t h = name_hash(rel);
    for (uint32_t i = 0; i < g_loose_n; i++) {
        if (g_loose[i].hash != h) continue;
        const int of = open("host:PS2_DATA/ovl/loose.ovl", O_RDONLY);
        if (of < 0) return -1;
        const int fd = io_alloc(of, g_loose[i].size);
        if (fd < 0) { close(of); return -1; }
        io_fd *f = &g_fd[fd];
        f->ovl_fd = of;                       /* all of it is "side": hl = r0 = rs = 0 */
        f->hl = f->r0 = f->rs = 0;
        f->side_at = g_loose[i].off;
        return fd;
    }
    return -1;
}

static void hle_IoOpenPath(void) {
    char p[160];
    psp_str(psp_arg(0), p, sizeof p);
    int fd = -1, hf = -1;
    uint32_t size = 0;
    if (io_prefix(p, "ms0:")) {
        fd = io_alloc(-1, 0);
    } else if (io_prefix(p, "disc0:")) {
        hf = io_host_open(p, &size);
        if (hf >= 0) {
            fd = io_alloc(hf, size);
            if (fd < 0) close(hf);
            else { io_ovl_attach(fd, p); size = g_fd[fd].size; }
        } else if ((fd = io_loose_open(p)) >= 0) {
            size = g_fd[fd].size;
        }
    }
    { extern void ps2_mpeg_note_open(const char *); ps2_mpeg_note_open(p); }   /* which movie */
    if (g_io_open_logged < 400) {
        g_io_open_logged++;
        psp_printf("[io] open \"%s\" -> fd=%d host=%d size=%u ra=0x%08X\n",
                   p, fd, hf, size, psp_cpu.r[PSP_REG_RA]);
    }
    psp_ret(fd >= 0 ? (uint32_t)fd : 0x80010002u);  /* NOENT */
}

static io_fd *io_get(uint32_t fd) {
    if (fd >= IO_FD_MAX || !g_fd[fd].used) return NULL;
    return &g_fd[fd];
}

static void hle_IoRead(void) {
    io_fd *f = io_get(psp_arg(0));
    const uint32_t buf = psp_arg(1);
    uint32_t n = psp_arg(2);
    if (!f) { psp_ret(0x80010005u); return; }
    void *dst = buf ? psp_mem_ptr(buf, n ? n : 1u) : NULL;
    if (!dst) {
        static uint32_t bad;
        if (bad < 5) { bad++; psp_printf("[io] read with bad buffer 0x%08X n=%u -> EFAULT\n", buf, n); }
        psp_ret(0x80010005u);
        return;
    }
    const uint32_t avail = f->size - f->off;
    if (n > avail) n = avail;
    ps2_io_total_reads++;
    int got = 0;
    if (f->host_fd >= 0 && n) got = io_vread(f, (uint8_t *)dst, f->off, n);
    if (g_io_reads < 400) {
        g_io_reads++;
        psp_printf("[io] read fd=%u off=%u n=%u -> %d\n", psp_arg(0), f->off, n, got);
    }
    f->off += (uint32_t)got;
    psp_ret((uint32_t)got);
}

static uint32_t g_io_lseeks;
static void hle_IoLseek(void) {
    io_fd *f = io_get(psp_arg(0));
    if (!f) { psp_ret(0x80010005u); return; }
    /* The 64-bit SceOff sits in an even register pair, so with fd in a0 the offset is
     * a2:a3 (reading a1 gave a stale guest pointer). whence is the fifth argument, and
     * PSP calls pass arguments 5-8 in $t0-$t3, not on the stack: reading sp+16 gave
     * stack garbage, so most seeks were taken as SEEK_END and landed at the file's end
     * (the MV screen then never found its image in all5.mp and retried forever). */
    const int64_t off = (int64_t)((uint64_t)psp_arg(2) | ((uint64_t)psp_arg(3) << 32));
    const uint32_t whence = psp_arg(4);
    int64_t base = whence == 0 ? 0 : (whence == 1 ? (int64_t)f->off : (int64_t)f->size);
    int64_t np = base + off;
    if (np < 0) np = 0;
    if (np > (int64_t)f->size) np = f->size;
    f->off = (uint32_t)np;
    if (g_io_lseeks < 10) {
        g_io_lseeks++;
        psp_printf("[io] lseek fd=%u off=%lld whence=%u -> %u ra=0x%08X\n",
                   psp_arg(0), (long long)off, whence, f->off, psp_cpu.r[PSP_REG_RA]);
    }
    psp_cpu.r[PSP_REG_V1] = (uint32_t)((uint64_t)np >> 32);   /* SceOff: v0:v1 */
    psp_ret((uint32_t)np);
}

static void async_store(uint32_t id, uint32_t result);

/* sceIoLseekAsync(fd, SceOff offset, whence): offset in a2:a3, whence in $t0 (as
 * sceIoLseek); the new position is the async result. */
static void hle_IoLseekAsync(void) {
    io_fd *f = io_get(psp_arg(0));
    if (!f) { psp_ret(0x80010005u); return; }
    const int64_t off = (int64_t)((uint64_t)psp_arg(2) | ((uint64_t)psp_arg(3) << 32));
    const uint32_t whence = psp_arg(4);
    int64_t np = (whence == 0 ? 0 : whence == 1 ? (int64_t)f->off : (int64_t)f->size) + off;
    if (np < 0) np = 0;
    if (np > (int64_t)f->size) np = f->size;
    f->off = (uint32_t)np;
    async_store(psp_arg(0), f->off);
    psp_ret(0);
}

static void hle_IoClose(void) {
    io_fd *f = io_get(psp_arg(0));
    if (f) {
        if (f->host_fd >= 0) close(f->host_fd);
        if (f->ovl_fd >= 0 && f->ovl_fd != f->host_fd) close(f->ovl_fd);
        f->used = 0;
    }
    psp_ret(0);
}

/* ---- async family -------------------------------------------------------
 * Same model as the tested host implementation: every operation happens now,
 * only the report is deferred. Ids for opens start at 0x1000 so they never
 * collide with real fds (3..7). The game uses WaitAsyncCB, not WaitAsync. */
#define MAX_ASYNC 64
static struct { uint32_t id, result; int used; } g_async[MAX_ASYNC];
static uint32_t g_async_next = 0x1000u;

static void async_store(uint32_t id, uint32_t result) {
    for (int i = 0; i < MAX_ASYNC; i++)
        if (g_async[i].used && g_async[i].id == id) { g_async[i].result = result; return; }
    for (int i = 0; i < MAX_ASYNC; i++)
        if (!g_async[i].used) { g_async[i].used = 1; g_async[i].id = id; g_async[i].result = result; return; }
}

static int async_take(uint32_t id, uint32_t *out) {
    for (int i = 0; i < MAX_ASYNC; i++)
        if (g_async[i].used && g_async[i].id == id) {
            *out = g_async[i].result;
            g_async[i].used = 0;
            return 1;
        }
    return 0;
}

static void hle_IoOpenAsync2(void) {
    hle_IoOpenPath();                       /* leaves fd or error in $v0 */
    const uint32_t fd = psp_cpu.r[PSP_REG_V0];
    const uint32_t id = g_async_next++;
    async_store(id, fd);
    psp_ret(id);
}

static void hle_IoReadAsync(void) {
    hle_IoRead();                           /* reads now, count in $v0 */
    const uint32_t n = psp_cpu.r[PSP_REG_V0];
    async_store(psp_arg(0), n);
    psp_ret(0);
}

/* The firmware reports an SceInt64: the game tests its high word. */
static void async_result(uint32_t dst, uint32_t value) {
    if (!dst) return;
    psp_write32(dst, value);
    psp_write32(dst + 4u, (int32_t)value < 0 ? 0xFFFFFFFFu : 0u);
}

static uint32_t g_io_waits;
static void hle_IoWaitAsync(void) {
    uint32_t id = psp_arg(0), dst = psp_arg(1), result = 0;
    if (!async_take(id, &result)) result = 0;   /* pending-on-fd: report 0, never -1 */
    async_result(dst, result);
    if (g_io_waits < 8) {
        g_io_waits++;
        psp_printf("[io] wait id=0x%X dst=0x%08X -> %u ra=0x%08X\n",
                   id, dst, result, psp_cpu.r[PSP_REG_RA]);
    }
    psp_ret(0);
}

static void hle_IoPollAsync(void) {
    uint32_t id = psp_arg(0), dst = psp_arg(1), result = 0;
    const int have = async_take(id, &result);
    async_result(dst, have ? result : 0u);
    psp_ret(0);
}

static void hle_IoCloseAsync(void) { hle_IoClose(); psp_ret(0); }
static void hle_IoSetAsyncCallback(void) { psp_ret(0); }

static uint32_t g_io_getstats;
static void hle_IoGetstat(void) {
    char p[160];
    psp_str(psp_arg(0), p, sizeof p);
    const uint32_t st = psp_arg(1);
    if (st) {
        /* SceIoStat is 0x58 bytes; st_size is a 64-bit value at 0x08. Zero the
         * whole struct first: leaving the timestamps/private area with stack
         * garbage is how a pointer ends up read as an offset. */
        for (uint32_t i = 0; i < 0x58; i += 4) psp_write32(st + i, 0);
        uint32_t size = 0;
        if (io_prefix(p, "disc0:")) {
            const int hf = io_host_open(p, &size);
            if (hf < 0) {
                const int lf = io_loose_open(p);
                if (lf < 0) { psp_ret(0x80010002u); return; }
                size = g_fd[lf].size;
                close(g_fd[lf].host_fd);
                g_fd[lf].used = 0;
                goto stat_size;
            }
            const int t = io_alloc(hf, size);         /* an overlaid pack reports its full size */
            if (t >= 0) {
                io_ovl_attach(t, p);
                size = g_fd[t].size;
                if (g_fd[t].ovl_fd >= 0) close(g_fd[t].ovl_fd);
                g_fd[t].used = 0;
            }
            close(hf);
        } else if (!io_prefix(p, "ms0:")) {
            psp_ret(0x80010002u);
            return;
        }
stat_size:
        psp_write32(st + 8, size);
        if (g_io_getstats < 6) {
            g_io_getstats++;
            psp_printf("[io] getstat \"%s\" size=%u\n", p, size);
        }
    }
    psp_ret(0);
}

static void hle_IoDopen(void) {
    char p[160];
    psp_str(psp_arg(0), p, sizeof p);
    if (g_io_open_logged < 10) {
        g_io_open_logged++;
        psp_printf("[io] dopen \"%s\"\n", p);
    }
    if (io_prefix(p, "ms0:")) { int fd = io_alloc(-1, 0); psp_ret(fd >= 0 ? (uint32_t)fd : 0x80010002u); return; }
    psp_ret(0x80010002u);
}

static void hle_IoError5(void)   { psp_ret(0x80010005u); } /* EBADF */

/* The game's own debug output (sceKernelStdout + sceIoWrite) goes to the log: it can say
 * what a screen is unhappy about. Other descriptors cannot be written (read-only disc). */
/* sceUtilityNetconf: the PS2 has no WLAN, so the connection dialog behaves as if the
 * player closed it: INIT -> VISIBLE -> QUIT with result 1 (cancelled), then
 * ShutdownStart -> FINISHED -> NONE. Without it GetStatus answered 0 forever and the
 * Network screen waited for a dialog that never ended. sceNet/sceNetAdhoc* init calls
 * stay unimplemented (they return 0 and nothing is ever connected). */
enum { DLG_NONE = 0, DLG_INIT, DLG_VISIBLE, DLG_QUIT, DLG_FINISHED };
static uint32_t g_netconf_status, g_netconf_param;
static void hle_NetconfInitStart(void) {
    if (g_netconf_status != DLG_NONE) { psp_ret(0x80110001u); return; }   /* busy */
    g_netconf_param = psp_arg(0);
    g_netconf_status = DLG_INIT;
    psp_printf("[utility] netconf start (no WLAN on the PS2: reported as cancelled)\n");
    psp_ret(0);
}
static void hle_NetconfGetStatus(void) {
    const uint32_t s = g_netconf_status;
    if (s == DLG_INIT) g_netconf_status = DLG_VISIBLE;
    else if (s == DLG_VISIBLE) {
        if (g_netconf_param && psp_mem_ptr(g_netconf_param, 32)) psp_write32(g_netconf_param + 28, 1u);  /* common.result */
        g_netconf_status = DLG_QUIT;
    } else if (s == DLG_FINISHED) g_netconf_status = DLG_NONE;
    psp_ret(s);
}
static void hle_NetconfUpdate(void) { psp_ret(0); }
static void hle_NetconfShutdownStart(void) {
    if (g_netconf_status != DLG_QUIT) { psp_ret(0x80110001u); return; }
    g_netconf_status = DLG_FINISHED;
    psp_ret(0);
}

/* sceNet/sceNetAdhoc: nobody is ever around. The calls that report data or peers say
 * "none" the way a PSP alone in the room does; the rest keep returning 0. PdpRecv
 * answering 0 ("received") had the lobby loop on phantom packets without drawing. */
#define NET_ADHOC_WOULD_BLOCK 0x80410709u
static void hle_NetGetLocalEtherAddr(void) {       /* (u8 mac[6]) */
    static const uint8_t mac[6] = { 0x00, 0x1F, 0xA7, 0x50, 0x53, 0x32 };   /* made up */
    for (int i = 0; i < 6; i++) psp_write8(psp_arg(0) + (uint32_t)i, mac[i]);
    psp_ret(0);
}
static void hle_AdhocPdpRecv(void) {              /* (id, mac, port*, buf, len*, timeout, nonblock) */
    if (psp_arg(4)) psp_write32(psp_arg(4), 0);
    psp_ret(NET_ADHOC_WOULD_BLOCK);
}
static void hle_AdhocPdpSend(void) { psp_ret(psp_arg(4)); }     /* (id, mac, port, data, len, ...): into the void */
static void hle_AdhocctlGetState(void) {           /* (int *state): 0 = disconnected */
    if (psp_arg(0)) psp_write32(psp_arg(0), 0);
    psp_ret(0);
}
static void hle_AdhocctlGetPeerList(void) {        /* (int *buflen, buf) */
    if (psp_arg(0)) psp_write32(psp_arg(0), 0);
    psp_ret(0);
}
static void hle_AdhocMatchingGetMembers(void) {    /* (id, int *buflen, buf) */
    if (psp_arg(1)) psp_write32(psp_arg(1), 0);
    psp_ret(0);
}

/* sceDmacMemcpy(dst, src, size). Into PSP VRAM it is the movie player putting a decoded
 * frame on a framebuffer (0x8BFC8: sceGeEdramGetAddr() + 0x88000): ps2_ge draws it.
 * sceGeEdramGetAddr answers 0 here (VRAM is not mapped; base 0 keeps the game's own
 * VRAM addresses on the module window as before), so VRAM is 0x04xxxxxx or below 2 MB. */
static void hle_DmacMemcpy(void) {
    const uint32_t dst = psp_arg(0), src = psp_arg(1), n = psp_arg(2);
    const uint32_t d = dst & PSP_ADDR_MASK;
    if ((d & 0xFF000000u) == 0x04000000u || d < 0x00200000u) {
        static uint32_t logged;
        if (logged++ < 2u) psp_printf("[dmac] frame to VRAM dst=0x%08X src=0x%08X size=%u\n", dst, src, n);
        ps2_ge_vram_copy(d & 0x001FFFFFu, src & PSP_ADDR_MASK, n);
    } else if (n) {
        void *d = psp_mem_ptr(dst, n);
        const void *s = psp_mem_ptr(src, n);
        if (d && s) memmove(d, s, n);
    }
    psp_ret(0);
}

static void hle_KernelStdin(void)  { psp_ret(0); }
static void hle_KernelStdout(void) { psp_ret(1); }
static void hle_KernelStderr(void) { psp_ret(2); }
static void hle_IoWrite(void) {
    const uint32_t fd = psp_arg(0), len = psp_arg(2);
    if (fd != 1u && fd != 2u) { psp_ret(0x80010005u); return; }
    static uint32_t lines;
    const char *p = len ? (const char *)psp_mem_ptr(psp_arg(1), len) : NULL;
    if (p && lines < 300u) {
        char buf[161];
        const uint32_t n = len < 160u ? len : 160u;
        memcpy(buf, p, n);
        buf[n] = 0;
        for (uint32_t i = 0; i < n; i++) if (buf[i] == '\n' || buf[i] == '\r') buf[i] = ' ';
        psp_printf("[stdout] %s\n", buf);
        lines++;
    }
    psp_ret(len);
}
static void hle_IoReturn0(void){ psp_ret(0); }
static void hle_IoReturnNeg(void){ psp_ret(0xFFFFFFFFu); }
static void hle_GeEnqueue(void) {
    static uint32_t n;
    if ((n++ % 60u) == 0u) {
        const uint32_t m = 0x002BC5ACu;
        psp_printf("[mgr] n=%u f0=%08X f4=%08X f8=%08X f12=%08X f32=%08X f44=%08X f72=%08X q0=%08X q1=%08X\n",
                   n, psp_read32(m), psp_read32(m+4), psp_read32(m+8), psp_read32(m+12),
                   psp_read32(m+32), psp_read32(m+44), psp_read32(m+72),
                   psp_read32(0x002C4F2Cu), psp_read32(0x002C4F30u));
        psp_printf("[mgr2] slot0_status=%08X\n", psp_read32(0x002BC6F0u));
    }
    psp_ret(ps2_ge_enqueue(psp_arg(0), psp_arg(1)));
}
static void hle_GeUpdateStall(void) { ps2_ge_update_stall(psp_arg(0), psp_arg(1)); psp_ret(0); }

/* ---- display: a monotonic vcount tied to guest progress -----------------
 * The boot waits for the counter to move; returning a constant parks it. It
 * advances on every query, on every vblank wait and on every frame buffer
 * hand-off, and never repeats a value. */
static uint32_t g_vcount = 1;
static uint32_t g_frame_no;

/* HOME/cerrar/reposo: sin aptMainLoop el botón HOME no responde en consola.
 * Se bombea como mucho una vez cada ~8 ms desde los puntos de frame. */
static void ps2_sys_pump(void) {
    static uint64_t last;
    const uint64_t now = ps2_time_us();
    if (now - last < 8000u) return;
    last = now;
    if (!aptMainLoop()) {
        extern void psp_stop_now(void);
        psp_printf("[sys] HOME: cerrando\n");
        psp_stop_now();
    }
}
static void hle_GetVcount(void)  { psp_ret(++g_vcount); }
static void hle_WaitVblank(void) { g_vcount++; ps2_sys_pump(); ps2_sched_yield(); psp_ret(0); }
static uint32_t g_fb_logged;
static void hle_SetFrameBuf(void){
    g_frame_no++;
    g_vcount++;
    ps2_sys_pump();
    if ((g_frame_no % 60u) == 0u) video_log_flush();
    ps2_ge_frame(psp_arg(0), psp_arg(3));
    if (g_fb_logged < 8) {
        g_fb_logged++;
        psp_printf("[disp] SetFrameBuf fb=0x%08X width=%u format=0x%X sync=%u\n",
                   psp_arg(0), psp_arg(1), psp_arg(2), psp_arg(3));
    }
    psp_ret(0);
}
static void hle_GetFramePerSec(void) { psp_ret(60); }

/* ---- sceAudio: canales PSP reales sobre el backend ndsp (audio_3ds.c) -----
 * Modelo del port PS2: el hardware encola UN buffer; una salida blocking espera
 * a que el anterior termine (reloj busy_until por canal) y encola. Así
 * GetChannelRestLength drena a 44.1 kHz real y el hilo de audio del juego
 * produce granos a la tasa correcta (sin esto, voces/UI salen ralentizadas). */
#define AUDIO_SAMPLE_MAX 4096u
#define AUDIO_LATE_OK 1024u            /* tarde pero contiguo: sin reset del reloj */
static struct {
    int resv, mono;
    uint32_t samples;
    uint64_t busy_until;               /* µs */
    uint64_t next_smp;                 /* reloj de muestras del canal */
} g_ach[PS2_AUDIO_CH];
static int16_t  g_atmp[AUDIO_SAMPLE_MAX * 2];
static uint32_t g_audio_logs, g_audio_calls, g_audio_nz;

static inline uint64_t aud_us2smp(uint64_t us) { return us * 441ull / 10000ull; }
static inline uint64_t aud_smp2us(uint64_t s)  { return s * 10000ull / 441ull; }

static void audio_out(int ch, uint32_t voll, uint32_t volr, uint32_t buf, int blocking) {
    if (ch < 0 || ch >= PS2_AUDIO_CH || !g_ach[ch].resv || !buf) { psp_ret(0x80260002u); return; }
    uint32_t n = g_ach[ch].samples ? g_ach[ch].samples : 1024u;
    if (n > AUDIO_SAMPLE_MAX) n = AUDIO_SAMPLE_MAX;
    const int mono = g_ach[ch].mono;
    const int16_t *s = (const int16_t *)psp_mem_ptr(buf, n * (mono ? 2u : 4u));
    if (!s) { psp_ret(0x80260002u); return; }
    if (!ps2_audio_ok()) {              /* sin backend: consumir y marcar el paso del tiempo */
        g_vcount += n / 735u;
        ps2_sched_yield();
        psp_ret(n);
        return;
    }
    uint64_t start = ps2_time_us();
    int contiguous = 0;
    if (g_ach[ch].busy_until > start) {
        if (!blocking) { psp_ret(0x80260002u); return; }   /* canal ocupado */
        /* Jitter buffer: despierta ~23 ms antes de que acabe el anterior; el
         * reloj (next_smp/busy_until) no cambia, solo la fase del bloqueo. */
        const uint64_t AHEAD_US = 23219u;
        start = g_ach[ch].busy_until;
        if (start > ps2_time_us() + AHEAD_US)
            ps2_audio_wait_until(start - AHEAD_US);
        contiguous = 1;
    } else if (g_ach[ch].next_smp && aud_us2smp(start) < g_ach[ch].next_smp + AUDIO_LATE_OK) {
        contiguous = 1;                 /* tarde, pero su hueco aun no se reprodujo */
    }
    g_ach[ch].busy_until = contiguous ? aud_smp2us(g_ach[ch].next_smp + n)
                                      : start + aud_smp2us(n);
    g_ach[ch].next_smp = (contiguous ? g_ach[ch].next_smp : aud_us2smp(start)) + n;
    if (voll > 0x8000u) voll = 0x8000u;
    if (volr > 0x8000u) volr = 0x8000u;
    uint32_t nz = 0;
    for (uint32_t i = 0; i < n; i++) {
        const int32_t l = mono ? s[i] : s[i * 2u];
        const int32_t r = mono ? s[i] : s[i * 2u + 1u];
        g_atmp[i * 2u]      = (int16_t)((l * (int32_t)voll) >> 15);
        g_atmp[i * 2u + 1u] = (int16_t)((r * (int32_t)volr) >> 15);
        nz |= (uint32_t)(l | r);
    }
    ps2_audio_push(ch, g_atmp, n);
    g_audio_calls++;
    if (nz) g_audio_nz++;
    {   /* tasa efectiva por canal: detecta contenidos ralentizados/acelerados */
        static uint64_t t0[PS2_AUDIO_CH];
        static uint64_t fr[PS2_AUDIO_CH];
        static uint32_t cn[PS2_AUDIO_CH], nzc[PS2_AUDIO_CH];
        const uint64_t now = ps2_time_us();
        if (!t0[ch]) t0[ch] = now;
        fr[ch] += n;
        if (nz) nzc[ch]++;
        if (++cn[ch] == 400u) {
            const uint64_t dt = now - t0[ch];
            uint32_t pf, wc;
            { extern void ps2_audio_diag(uint32_t *, uint32_t *); ps2_audio_diag(&pf, &wc); }
            uint32_t ur;
            { extern uint32_t ps2_audio_underruns(int); ur = ps2_audio_underruns(ch); }
            psp_printf("[adiag] ch=%d Hz=%u nz=%u/400 len=%u preempt=%u waits=%u under=%u\n", ch,
                       dt ? (uint32_t)(fr[ch] * 1000000ull / dt) : 0u, nzc[ch], n, pf, wc, ur);
            t0[ch] = now; fr[ch] = 0; cn[ch] = nzc[ch] = 0;
        }
    }
    if (g_audio_logs < 6 && (g_audio_calls == 1 || g_audio_calls == 100 || g_audio_calls == 1000)) {
        g_audio_logs++;
        psp_printf("[audio] out ch=%d n=%u vol=%u/%u mono=%d calls=%u nz=%u level=%u\n",
                   ch, n, voll, volr, mono, g_audio_calls, g_audio_nz, ps2_audio_level(ch));
    }
    psp_ret(n);
}
static void hle_AudioOutputBlocking(void)       { audio_out((int)psp_arg(0), psp_arg(1), psp_arg(1), psp_arg(2), 1); }
static void hle_AudioOutput(void)               { audio_out((int)psp_arg(0), psp_arg(1), psp_arg(1), psp_arg(2), 0); }
static void hle_AudioOutputPannedBlocking(void) { audio_out((int)psp_arg(0), psp_arg(1), psp_arg(2), psp_arg(3), 1); }
static void hle_AudioOutputPanned(void)         { audio_out((int)psp_arg(0), psp_arg(1), psp_arg(2), psp_arg(3), 0); }
static void hle_AudioChReserve(void) {
    int ch = (int)psp_arg(0);
    const uint32_t samples = psp_arg(1), format = psp_arg(2);
    if (ch < 0) { for (ch = 0; ch < PS2_AUDIO_CH && g_ach[ch].resv; ch++) { } }
    if (ch >= PS2_AUDIO_CH || samples < 64u || samples > AUDIO_SAMPLE_MAX || (samples & 63u)) {
        psp_ret(0x80260001u);
        return;
    }
    g_ach[ch].resv = 1;
    g_ach[ch].samples = samples;
    g_ach[ch].mono = format == 0x10u;
    g_ach[ch].busy_until = 0;
    g_ach[ch].next_smp = 0;
    ps2_audio_reset(ch);
    psp_printf("[audio] ChReserve ch=%d samples=%u format=%u\n", ch, samples, format);
    psp_ret((uint32_t)ch);
}
static void hle_AudioChRelease(void) {
    const int ch = (int)psp_arg(0);
    if (ch >= 0 && ch < PS2_AUDIO_CH) {
        g_ach[ch].resv = 0;
        g_ach[ch].next_smp = 0;
        ps2_audio_reset(ch);
    }
    psp_ret(0);
}
static void hle_AudioSetChannelDataLen(void) {
    const int ch = (int)psp_arg(0);
    if (ch >= 0 && ch < PS2_AUDIO_CH && psp_arg(1) <= AUDIO_SAMPLE_MAX) {
        static uint32_t chg;
        if (g_ach[ch].samples != psp_arg(1) && chg < 12u) { chg++;
            psp_printf("[adiag] DataLen ch=%d %u->%u\n", ch, g_ach[ch].samples, psp_arg(1)); }
        g_ach[ch].samples = psp_arg(1);
    }
    psp_ret(0);
}
static void hle_AudioRestLen(void) {
    const int ch = (int)psp_arg(0);
    if (ch < 0 || ch >= PS2_AUDIO_CH) { psp_ret(0); return; }
    const uint64_t now = ps2_time_us();
    psp_ret(g_ach[ch].busy_until > now
            ? (uint32_t)aud_us2smp(g_ach[ch].busy_until - now) : 0u);
}

/* ---- controller: pad 1 through the ROM's PADMAN ---------------------------
 * Face buttons, d-pad, start and select use the same bits on PSP and PS2. */
static uint8_t g_padbuf[256] __attribute__((aligned(64)));
static int     g_pad_ok;

/* Test driver: host:autopad.txt holds "<ms since boot> <hex PSP buttons>" lines, each
 * held until the next, so a run can walk the menus unattended. Absent in normal use.
 * The "[pad] t=<ms>" log lines of a played session are in this format, so a session
 * can be replayed by copying them (time, not read count: the read rate follows fps).
 * ponytail: host: exists only in the emulator; a disc build should compile this out. */
#define AUTOPAD_MAX 1024
static struct { uint32_t at, buttons; } g_autopad[AUTOPAD_MAX];
static int      g_autopad_n;
static uint32_t g_chart_origin_ms;
static uint32_t g_chart_pause_ms;
static uint32_t g_ctrl_reads;

void ps2_pad_chart_start(void) {
    if (!g_autopad_n) return;
    g_chart_origin_ms = (uint32_t)(ps2_time_us()/1000u);
    g_chart_pause_ms = 0;
    psp_printf("[pad] chart origin=%u ms\n", g_chart_origin_ms);
}
void ps2_pad_chart_pause(void) {
    if (g_autopad_n && g_chart_origin_ms && !g_chart_pause_ms)
        g_chart_pause_ms=(uint32_t)(ps2_time_us()/1000u);
}
void ps2_pad_chart_resume(void) {
    if (g_chart_pause_ms) {
        uint32_t now=(uint32_t)(ps2_time_us()/1000u);
        g_chart_origin_ms+=now-g_chart_pause_ms;
        psp_printf("[pad] chart resumed, pause=%u ms\n", now-g_chart_pause_ms);
        g_chart_pause_ms=0;
    }
}

static void gedump_init(void);
void ps2_pad_init(void) {
    SifInitRpc(0);
    const int a = SifLoadModule("rom0:SIO2MAN", 0, NULL);
    const int b = SifLoadModule("rom0:PADMAN", 0, NULL);
    g_pad_ok = a >= 0 && b >= 0 && padInit(0) == 1 && padPortOpen(0, 0, g_padbuf) != 0;
    psp_printf("[pad] sio2man=%d padman=%d ok=%d\n", a, b, g_pad_ok);

    gedump_init();
    static char text[16384];
    int fd = open("host:autopad.txt", O_RDONLY);
    if (fd < 0) fd = open("sdmc:/djmx_autopad.txt", O_RDONLY);   /* solo pruebas: nombre explícito */
    if (fd < 0) return;
    const int len = (int)read(fd, text, sizeof text - 1);
    close(fd);
    if (len <= 0) return;
    text[len] = 0;
    for (char *p = text; g_autopad_n < AUTOPAD_MAX; ) {
        char *e;
        const unsigned long at = strtoul(p, &e, 10);
        if (e == p) break;
        p = e;
        g_autopad[g_autopad_n].at = (uint32_t)at;
        g_autopad[g_autopad_n].buttons = (uint32_t)strtoul(p, &e, 16);
        if (e == p) break;
        p = e;
        g_autopad_n++;
    }
    psp_printf("[pad] autopad: %d steps\n", g_autopad_n);
}

/* TEMP: host:gedump.txt lists ms times at which to dump one GE frame (R3 does it too). */
static uint32_t g_dump_at[16];
static int g_dump_n, g_dump_next;
static void gedump_init(void) {
    static char text[256];
    const int fd = open("host:gedump.txt", O_RDONLY);
    if (fd < 0) return;
    const int len = (int)read(fd, text, sizeof text - 1);
    close(fd);
    if (len <= 0) return;
    text[len] = 0;
    for (char *p = text, *e; g_dump_n < 16; p = e) {
        const unsigned long at = strtoul(p, &e, 10);
        if (e == p) break;
        g_dump_at[g_dump_n++] = (uint32_t)at;
    }
    psp_printf("[pad] gedump: %d times\n", g_dump_n);
}

/* Left stick -> the PSP's analog nub (0..255, 128 centre). The game turns it into menu
 * directions itself (0x82CE0: past 64/192, with auto-repeat). A DualShock starts in
 * digital mode, so each time a pad becomes stable it is switched to analog and locked
 * there; a digital-only pad (or the switch still in progress) reports the centre. */
static uint8_t g_lx = 128, g_ly = 128;
static uint32_t g_r3_now;
static uint32_t pad_buttons(void) {
    uint32_t out = 0;
    g_lx = g_ly = 128;
    if (g_pad_ok) {
        static int analog_asked;
        const int st = padGetState(0, 0);
        struct padButtonStatus b;
        if (st == PAD_STATE_DISCONN) analog_asked = 0;
        if (st == PAD_STATE_STABLE && !analog_asked) {
            analog_asked = 1;
            padSetMainMode(0, 0, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);
        }
        if ((st == PAD_STATE_STABLE || st == PAD_STATE_FINDCTP1) && padRead(0, 0, &b)) {
            const uint32_t on = 0xFFFFu ^ b.btns;       /* active low */
            g_r3_now = on & PAD_R3;
            out = on & 0xF0F9u;
            if (on & (PAD_L1 | PAD_L2)) out |= 0x0100u;  /* PSP L */
            if (on & (PAD_R1 | PAD_R2)) out |= 0x0200u;  /* PSP R */
            if ((b.mode >> 4) == 0x7u) { g_lx = b.ljoy_h; g_ly = b.ljoy_v; }   /* 0x73: analog */
        }
    }
    g_ctrl_reads++;
    const uint32_t ms = (uint32_t)(ps2_time_us() / 1000u);
    {   /* TEMP: frame dump on R3 (unused by the PSP mapping) or at a listed time */
        static uint32_t r3_was;
        const uint32_t r3 = g_pad_ok ? g_r3_now : 0u;
        if (r3 && !r3_was) ps2_ge_dump_request();
        r3_was = r3;
        if (g_dump_next < g_dump_n && ms >= g_dump_at[g_dump_next]) { g_dump_next++; ps2_ge_dump_request(); }
    }
    uint32_t absolute=0,relative=0,best_abs=0,best_rel=0;
    for (int i=0; i<g_autopad_n; i++) {
        uint32_t at=g_autopad[i].at;
        if (at & 0x80000000u) {
            at &= 0x7FFFFFFFu;
            if (g_chart_origin_ms && !g_chart_pause_ms && ms>=g_chart_origin_ms &&
                ms-g_chart_origin_ms>=at && at>=best_rel) {
                best_rel=at;relative=g_autopad[i].buttons;
            }
        } else if (ms>=at && at>=best_abs) {
            best_abs=at;absolute=g_autopad[i].buttons;
        }
    }
    out |= absolute | relative;
    return out;
}

/* Headphone remote (sceHprm): none attached, nothing pressed. The OST player reads the
 * latch every frame; left unwritten, its stack garbage was taken for remote buttons. */
static void hle_HprmLatch(void) {            /* Peek/ReadLatch(u32 latch[4]) */
    const uint32_t p = psp_arg(0);
    if (p && psp_mem_ptr(p, 16)) for (uint32_t i = 0; i < 16u; i += 4u) psp_write32(p + i, 0);
    psp_ret(0);
}
static void hle_HprmCurrentKey(void) {       /* PeekCurrentKey(u32 *key) */
    const uint32_t p = psp_arg(0);
    if (p && psp_mem_ptr(p, 4)) psp_write32(p, 0);
    psp_ret(0);
}
static void hle_HprmNone(void) { psp_ret(0); } /* Is{Remote,Headphone,Microphone}Exist */

/* sceCtrlReadBufferPositive(SceCtrlData *pad, int count) */
static void hle_CtrlRead(void) {
    const uint32_t buf = psp_arg(0);
    uint32_t buttons = pad_buttons();
    static uint32_t last, logged;
    if (buttons != last && logged < 4000) {     /* the player's input only: replayable */
        logged++;
        psp_printf("[pad] t=%u %04X\n", (uint32_t)(ps2_time_us() / 1000u), buttons);
    }
    last = buttons;
    if (buf && psp_mem_ptr(buf, 16)) {
        psp_write32(buf + 0, g_vcount);
        psp_write32(buf + 4, buttons);
        psp_write8 (buf + 8, g_lx);
        psp_write8 (buf + 9, g_ly);
        for (uint32_t i = 10; i < 16; i++) psp_write8(buf + i, 0);
    }
    psp_ret(1);
}

/* ---- sceMpeg: make the opening movie end immediately --------------------
 * A real AVC decoder is a separate project. The player is given a coherent
 * object, but GetAvcAu/GetAtracAu report "end of stream" on the first call,
 * which is the branch that closes the movie and returns to the game. */
static uint32_t g_ring_size = 0x40000u;
static uint32_t g_ring_used;
static uint32_t g_mpeg_logged;
#define MPEG_RING_PACKETS 0xF00u
static uint32_t g_mpeg_ring_cap = MPEG_RING_PACKETS;
uint32_t g_mpeg_ring_obj;
static uint32_t g_mpeg_ring_used;

/* ---- coherent sceMpeg init (analysis/3ds-video-hilo.md §9-A) -------------
 * The pump at 0x8B230 only reaches its clean error exit (0x8B500 -> 0x8B514)
 * if the init builds the player object: Init/Construct/Create must return 0,
 * RegistStream must return a non-null handle, QueryAtracEsSize must be > 0,
 * and GetAtracAu must return EOS (0x80618005, not 0x80618001). */
static void hle_MpegInitOk(void) { psp_ret(0); }

static void hle_MpegCreate(void) {
    /* ps2_mpeg.c: the "handle" is the data pointer, and the ring records it. */
    if (psp_arg(0) && psp_mem_ptr(psp_arg(0), 4)) psp_write32(psp_arg(0), psp_arg(1));
    if (g_mpeg_ring_obj && psp_mem_ptr(g_mpeg_ring_obj, 44))
        psp_write32(g_mpeg_ring_obj + 40, psp_arg(1));
    psp_printf("[mpeg] Create handle:=0x%08X -> 0\n", psp_arg(1));
    psp_ret(0);
}

static void hle_MpegRingbufConstruct(void) {
    const uint32_t ring = psp_arg(0);
    const uint32_t packets = psp_arg(1) ? psp_arg(1) : MPEG_RING_PACKETS;
    const uint32_t data = psp_arg(2);
    const uint32_t size = psp_arg(3);
    const uint32_t cb = psp_arg(4);
    const uint32_t param = psp_arg(5);
    if (ring && psp_mem_ptr(ring, 44)) {
        psp_write32(ring + 0,  packets);
        psp_write32(ring + 4,  0);
        psp_write32(ring + 8,  0);
        psp_write32(ring + 12, packets);
        psp_write32(ring + 16, 2048u);
        psp_write32(ring + 20, data);
        psp_write32(ring + 24, cb);
        psp_write32(ring + 28, param);
        psp_write32(ring + 32, data + packets * 2048u);
        psp_write32(ring + 36, 0xFFFFFFFFu);
        psp_write32(ring + 40, 0);
    }
    g_mpeg_ring_cap = packets;
    g_mpeg_ring_used = 0;
    extern uint32_t g_mpeg_ring_obj;
    g_mpeg_ring_obj = ring;
    psp_printf("[mpeg] Construct ring=0x%08X packets=%u data=0x%08X size=%u cb=0x%08X\n",
               ring, packets, data, size, cb);
    psp_ret(0);
}

static void hle_MpegRegistStream(void) { psp_ret(1); }   /* non-null stream handle */

static void hle_MpegQueryAtracEsSize(void) {
    if (psp_arg(1) && psp_mem_ptr(psp_arg(1), 4)) psp_write32(psp_arg(1), 0x2000u);
    if (psp_arg(2) && psp_mem_ptr(psp_arg(2), 4)) psp_write32(psp_arg(2), 0x2000u);
    psp_ret(0);
}

static uint32_t g_avail_q;
static int g_avail_logs;

static void hle_MpegRingbufAvailable(void) {
    g_avail_q++;
    const uint32_t free_bytes = (g_mpeg_ring_used >= g_mpeg_ring_cap)
        ? 0u : (g_mpeg_ring_cap - g_mpeg_ring_used) * 2048u;
    if (g_avail_logs < 6) { g_avail_logs++;
        psp_printf("[mpeg] avail used=%u/%u -> %u\n", g_mpeg_ring_used, g_mpeg_ring_cap, free_bytes); }
    psp_ret(free_bytes);
}
static void hle_MpegQueryMemSize(void) { psp_ret(0x10000u); }
static void hle_MpegEos(void) { psp_ret(0x80618005u); }    /* no more AUs */
static void hle_MpegLogEos(const char *what) {
    static int n;
    if (n < 4) { n++; psp_printf("[mpeg] %s -> EOS (closing movie)\n", what); }
    psp_ret(0x80618005u);
}
static uint32_t g_mv, g_ma; static int g_mact;
static void mpeg_au_write(uint32_t au, uint32_t pts) {
    if (!au || !psp_mem_ptr(au, 24)) return;
    psp_write32(au+0,0); psp_write32(au+4,pts); psp_write32(au+8,0); psp_write32(au+12,pts);
}
static void hle_MpegGetAvcAu(void) {
    if (!g_mact) { g_mact = 1; psp_printf("[mpeg] timing model on\n"); }
    if (g_mv >= 2629u) { psp_ret(0x80618001u); return; }
    mpeg_au_write(psp_arg(2), 90000u + 3003u * g_mv++);
    if (psp_arg(3) && psp_mem_ptr(psp_arg(3), 4)) psp_write32(psp_arg(3), 1);
    psp_ret(0);
}
static void hle_MpegGetAtracAu(void) {
    if (g_ma >= 1891u) { psp_ret(0x80618001u); return; }
    mpeg_au_write(psp_arg(2), 90000u + 4180u * g_ma++);
    if (psp_arg(3) && psp_mem_ptr(psp_arg(3), 4)) psp_write32(psp_arg(3), 0);
    psp_ret(0);
}
static void hle_MpegAvcDecode(void) {
    static uint32_t n;
    if (n < 4) { n++;
        psp_printf("[avc] a0=%08X a1=%08X a2=%08X a3=%08X a4=%08X\n",
                   psp_arg(0), psp_arg(1), psp_arg(2), psp_arg(3), psp_arg(4)); }
    if (psp_arg(4) && psp_mem_ptr(psp_arg(4), 4)) psp_write32(psp_arg(4), 1);
    psp_ret(0);
}
static void hle_MpegAtrac(void) { psp_ret(0); }
static void hle_MpegFail(void) { psp_ret(0x80618001u); }   /* "no AU yet, retry" */
static void hle_MpegEsBuf(void) { psp_ret(0x00030000u); }  /* fake ES buffer handle */

static void hle_MpegRingbufPut(void) {
    uint32_t n = psp_arg(1);
    if (!n) n = 0x20u;
    uint32_t acc = g_mpeg_ring_cap - g_mpeg_ring_used;
    if (acc > n) acc = n;
    g_mpeg_ring_used += acc;
    if (g_mpeg_logged < 6) {
        g_mpeg_logged++;
        psp_printf("[mpeg] Put ring=0x%08X n=%u accepted=%u used=%u/%u\n",
                   psp_arg(0), n, acc, g_mpeg_ring_used, g_mpeg_ring_cap);
    }
    psp_ret(acc);
    ps2_sched_yield();
}
/* sceKernelExitGame: log who asked for it, once. This is the branch that ends
 * the run before any frame is drawn, so its caller is the symptom to chase. */
static uint32_t g_exitgame_logged;
static void hle_ExitGame(void) {
    if (!g_exitgame_logged && ps2_sched_in_thread()) {
        g_exitgame_logged = 1;
        psp_printf("!! sceKernelExitGame from ra=0x%08X a0=0x%08X a1=0x%08X\n",
                   psp_cpu.r[PSP_REG_RA], psp_cpu.r[PSP_REG_A0],
                   psp_cpu.r[PSP_REG_A1]);
        psp_printf("   last NIDs:");
        for (int k = 7; k >= 0; k--) {
            const uint32_t n = psp_hle_last_nid(k);
            if (n) psp_printf(" %X", n);
        }
        psp_printf("\n");
    }
    psp_ret(0);
}
static void hle_UmdDriveStat(void) { psp_ret(0x20u); }   /* UMD ready */
static void hle_UmdWaitStat(void)  { ps2_sched_yield(); psp_ret(0); }
static void hle_GeSync(void)    { psp_ret(0); }

/* ======================================================================
 * SAS audio core (sceSasCore): mezclador real de voces VAG/ADPCM.
 * Los keysounds de DJMAX son voces SAS: cada __sceSasCore decodifica y mezcla
 * las voces activas en el búfer de grano del guest, que el juego envía después
 * por sceAudioOutput*. ADSR se aproxima a ganancia plena (one-shots).
 * ==================================================================== */
#define SAS_MAX_VOICES 32
typedef struct {
    uint32_t addr, size, off;      /* flujo VAG en RAM del guest */
    uint32_t loop_start;
    int      loop_en, playing;
    int      hist1, hist2;
    int16_t  buf[28];
    uint32_t bi;                   /* índice en el bloque decodificado */
    uint32_t frac;                 /* acumulador de pitch, base 0x1000 */
    int16_t  cur;
    uint32_t pitch;
    int      voll, volr;           /* 0..0x1000 */
} sas_voice;
static struct {
    int      used;
    uint32_t sas;
    uint32_t grain, voices, mode, rate;
    sas_voice v[SAS_MAX_VOICES];
    uint32_t rev_type, rev_von, rev_el, rev_er, rev_param;
} g_sas;
static uint32_t g_sas_logs;
static uint32_t g_sas_cores, g_sas_keyons, g_sas_nzmix;

/* Un bloque VAG: 16 bytes -> 28 muestras. flags: 6 marca inicio de loop,
 * 3 fin de loop (salta o termina), 1/7 último bloque. */
static int sas_block(sas_voice *v) {
    if (v->off + 16u > v->size) { v->playing = 0; return 0; }
    const uint8_t *b = (const uint8_t *)psp_mem_ptr(v->addr + v->off, 16);
    if (!b) { v->playing = 0; return 0; }
    const int pred = b[0] >> 4, shift = b[0] & 15, flags = b[1];
    static const int c0[5] = { 0, 60, 115, 98, 122 };
    static const int c1[5] = { 0, 0, -52, -55, -60 };
    const int f0 = pred < 5 ? c0[pred] : 0, f1 = pred < 5 ? c1[pred] : 0;
    if (flags == 6) v->loop_start = v->off;
    v->off += 16u;
    for (int i = 0; i < 28; i++) {
        const int nib = (i & 1) ? (b[2 + (i >> 1)] >> 4) : (b[2 + (i >> 1)] & 15);
        int s = (int16_t)(nib << 12) >> shift;
        s += (v->hist1 * f0 + v->hist2 * f1) >> 6;
        if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
        v->hist2 = v->hist1; v->hist1 = s;
        v->buf[i] = (int16_t)s;
    }
    v->bi = 0;
    if (flags == 1 || flags == 7) v->size = v->off;            /* último bloque */
    else if (flags == 3) { if (v->loop_en) v->off = v->loop_start; else v->size = v->off; }
    return 1;
}
static inline int16_t sas_next(sas_voice *v) {
    if (v->bi >= 28u && !sas_block(v)) return 0;
    return v->buf[v->bi++];
}
static void sas_mix(int16_t *out, uint32_t frames, int add, int dry_l, int dry_r) {
    uint32_t nz = 0;
    for (uint32_t i = 0; i < frames; i++) {
        int L = add ? ((int)out[i * 2u] * dry_l) >> 12 : 0;
        int R = add ? ((int)out[i * 2u + 1u] * dry_r) >> 12 : 0;
        for (uint32_t k = 0; k < g_sas.voices && k < SAS_MAX_VOICES; k++) {
            sas_voice *v = &g_sas.v[k];
            if (!v->playing) continue;
            v->frac += v->pitch;
            while (v->frac >= 0x1000u) {
                v->frac -= 0x1000u;
                v->cur = sas_next(v);
                if (!v->playing) { v->cur = 0; break; }
            }
            L += ((int)v->cur * v->voll) >> 12;
            R += ((int)v->cur * v->volr) >> 12;
        }
        if (L > 32767) L = 32767; else if (L < -32768) L = -32768;
        if (R > 32767) R = 32767; else if (R < -32768) R = -32768;
        out[i * 2u]      = (int16_t)L;
        out[i * 2u + 1u] = (int16_t)R;
        nz |= (uint32_t)(L | R);
    }
    if (nz) g_sas_nzmix++;
}

static void hle_SasInit(void) {
    if (!g_sas.used) { memset(&g_sas, 0, sizeof g_sas); g_sas.used = 1; }
    g_sas.sas    = psp_arg(0);
    g_sas.grain  = psp_arg(1);
    g_sas.voices = psp_arg(2);
    g_sas.mode   = psp_arg(3);
    g_sas.rate   = psp_arg(4);
    if (g_sas.voices < 1) g_sas.voices = SAS_MAX_VOICES;
    if (g_sas.voices > SAS_MAX_VOICES) g_sas.voices = SAS_MAX_VOICES;
    if (g_sas.grain > 2048u) g_sas.grain = 2048u;
    if (g_sas_logs < 4) {
        g_sas_logs++;
        psp_printf("[sas] init sas=0x%08X grain=%u voices=%u mode=%u rate=%u -> 0\n",
                   g_sas.sas, g_sas.grain, g_sas.voices, g_sas.mode, g_sas.rate);
    }
    psp_ret(0);
}

static void sas_core_common(uint32_t out, int add, int dry_l, int dry_r) {
    const uint32_t grain = g_sas.grain ? g_sas.grain : 1024u;
    int16_t *p = out ? (int16_t *)psp_mem_ptr(out, grain * 4u) : NULL;
    if (p) sas_mix(p, grain, add, dry_l, dry_r);
    g_sas_cores++;
    if (g_sas_cores == 1 || g_sas_cores == 1000 || g_sas_cores == 10000 || g_sas_cores == 30000) {
        psp_printf("[sas] core #%u out=0x%08X grain=%u keyons=%u nzmix=%u\n",
                   g_sas_cores, out, grain, g_sas_keyons, g_sas_nzmix);
    }
    psp_ret(0);
    ps2_sched_yield();
}
static void hle_SasCore(void)        { sas_core_common(psp_arg(1), 0, 0x1000, 0x1000); }
static void hle_SasCoreWithMix(void) { sas_core_common(psp_arg(1), 1, (int)psp_arg(2), (int)psp_arg(3)); }

static int sas_voice_idx(void) {
    const uint32_t v = psp_arg(1);
    return v < SAS_MAX_VOICES ? (int)v : -1;
}
static void hle_SasSetVoice(void) {      /* (sas, voice, vagAddr, size, loopmode) */
    const int i = sas_voice_idx();
    if (i >= 0) {
        sas_voice *v = &g_sas.v[i];
        const uint32_t addr = psp_arg(2), size = psp_arg(3);
        v->addr = addr; v->size = size;
        v->loop_en = (int)psp_arg(4) > 0;
        v->loop_start = 0;
        if (!addr || !size || !psp_mem_ptr(addr, size < 16u ? size : 16u)) {
            v->playing = 0; v->size = 0;
        }
    }
    psp_ret(0);
}
static void hle_SasSetPitch(void) {
    const int i = sas_voice_idx();
    if (i >= 0) {
        uint32_t p = psp_arg(2);
        if (p < 1u) p = 1u;
        if (p > 0x4000u) p = 0x4000u;
        if (p != 0x1000u) {
            static uint32_t odd;
            if (odd < 8u) { odd++; psp_printf("[adiag] sas pitch v=%d 0x%X\n", i, p); }
        }
        g_sas.v[i].pitch = p;
    }
    psp_ret(0);
}
static void hle_SasSetVolume(void) {     /* (sas, voice, l, r, el, er) */
    const int i = sas_voice_idx();
    if (i >= 0) {
        int l = (int)psp_arg(2), r = (int)psp_arg(3);
        if (l < 0) l = -l; if (r < 0) r = -r;
        if (l > 0x1000) l = 0x1000;
        if (r > 0x1000) r = 0x1000;
        g_sas.v[i].voll = l; g_sas.v[i].volr = r;
    }
    psp_ret(0);
}
static void hle_SasKeyOn(void) {
    const int i = sas_voice_idx();
    if (i >= 0 && g_sas.v[i].size) {
        sas_voice *v = &g_sas.v[i];
        v->off = 0; v->bi = 28u; v->frac = 0x1000u;
        v->hist1 = v->hist2 = 0; v->cur = 0;
        if (!v->pitch) v->pitch = 0x1000u;
        if (!v->voll && !v->volr) { v->voll = v->volr = 0x1000; }
        v->playing = 1;
        g_sas_keyons++;
        if (g_sas_keyons <= 4)
            psp_printf("[sas] keyon v=%d addr=0x%08X size=%u pitch=0x%X vol=%d/%d\n",
                       i, v->addr, v->size, v->pitch, v->voll, v->volr);
    }
    psp_ret(0);
}
static void hle_SasKeyOff(void) {
    const int i = sas_voice_idx();
    if (i >= 0) g_sas.v[i].playing = 0;   /* aproximación: corte en vez de release */
    psp_ret(0);
}
static void hle_SasOk(void)     { psp_ret(0); }
static void hle_SasGetEndFlag(void) {
    uint32_t flags = 0;
    for (uint32_t v = 0; v < g_sas.voices && v < SAS_MAX_VOICES; v++)
        if (!g_sas.v[v].playing) flags |= 1u << v;   /* 1 = voz terminada/libre */
    psp_ret(flags);
}
static void hle_SasGetZero(void) { psp_ret(0); }
static void hle_SasGetEnvelopeHeight(void) {
    const int i = sas_voice_idx();
    psp_ret(i >= 0 && g_sas.v[i].playing ? 0x40000000u : 0u);
}
static void hle_SasRevType(void) { g_sas.rev_type = psp_arg(1); psp_ret(0); }
static void hle_SasRevVON(void)  { g_sas.rev_von  = psp_arg(1); psp_ret(0); }
static void hle_SasRevEVOL(void) { g_sas.rev_el = psp_arg(1); g_sas.rev_er = psp_arg(2); psp_ret(0); }
static void hle_SasRevParam(void){ g_sas.rev_param = psp_arg(3); psp_ret(0); }

/* ---- remaining system NIDs (identified by SHA-1 against psp_imports.csv) -- */
static uint32_t g_sysparam_logs;
static void hle_UtilityGetSystemParamInt(void) {
    const uint32_t id = psp_arg(0), out = psp_arg(1);
    uint32_t val = 0;
    switch (id) {
    case 1:  val = 1;  break;      /* language: English */
    case 2:  val = 0;  break;      /* backlight */
    case 3:  val = 0;  break;      /* ??? */
    case 4:  val = 0;  break;
    default: val = 0;  break;
    }
    (void)val; (void)out;   /* PS2 stub returned 0 without touching memory */
    if (g_sysparam_logs < 8) { g_sysparam_logs++;
        psp_printf("[sys] UtilityGetSystemParamInt id=%u -> 0 (no write)\n", id); }
    psp_ret(0);
}
static void hle_ImposeSetLanguageMode(void) { psp_ret(0); }
static uint32_t g_utilmod_logs;
static void hle_UtilityLoadModule(void) {
    if (g_utilmod_logs < 8) { g_utilmod_logs++;
        psp_printf("[sys] UtilityLoadModule 0x%X -> 0\n", psp_arg(0)); }
    psp_ret(0);
}
static void hle_GeUnsetCallback(void) { psp_ret(0); }
static void hle_PowerUnknown(void) { psp_ret(0); }   /* PS2 returned 0 */

/* ---- wait/signal trace: who blocks on what, and who wakes it -------------- */
static uint32_t g_wait_logs, g_sig_logs;
static void log_wait(const char *what, uint32_t uid) {
    if (g_wait_logs < 300) {
        g_wait_logs++;
        psp_printf("[wait] %s th=0x%X uid=0x%X ra=0x%08X\n", what,
                   ps2_sched_current_uid(), uid, psp_cpu.r[PSP_REG_RA]);
    }
}
static void log_sig(const char *what, uint32_t uid, uint32_t val) {
    if (g_sig_logs < 300) {
        g_sig_logs++;
        psp_printf("[sig] %s th=0x%X uid=0x%X val=0x%X ra=0x%08X\n", what,
                   ps2_sched_current_uid(), uid, val, psp_cpu.r[PSP_REG_RA]);
    }
}
/* ---- sceAtrac3plus: decodificación real en atrac_3ds.c (ps2_atrac.h + at3) */
void ps2_atrac_install(void);

/* ---- Utility / RTC: dialogs report "finished" so no frame loop waits ---- */
/* ---- sceUtility savedata + msg dialog: protocolo real -----------------------
 * El juego guarda el perfil al confirmar el nombre: InitStart hace el trabajo
 * al instante (archivo en sdmc:/djmax/SAVE/), y GetStatus recorre 2 -> 3 para
 * que la máquina de estados del juego termine. Sin esto, el registro del
 * perfil se queda esperando para siempre. */
static int g_sd_status, g_sd_calls;          /* savedata:  0 none, 1 running */
static int g_md_status, g_md_calls;          /* msgdialog: idem */

static void savedata_path(char *out, size_t cap, uint32_t param) {
    char game[16] = {0}, save[24] = {0}, file[16] = {0};
    psp_str(param + 60u, game, 14);
    psp_str(param + 76u, save, 21);
    psp_str(param + 100u, file, 14);
    if (!file[0]) snprintf(file, sizeof file, "DATA.BIN");
    snprintf(out, cap, "sdmc:/djmax/SAVE/%s%s_%s", game, save, file);
    /* la SD del emulador es un directorio de Windows: sin <>:"/\|?* */
    for (char *p = out + 5; *p; p++)
        if (strchr("<>:\"\\|?*", *p)) *p = '_';
}
static void hle_UtilitySaveInit(void) {
    const uint32_t param = psp_arg(0);
    uint32_t mode = 0, databuf = 0, bufsize = 0, datasize = 0, result = 0;
    char path[160] = {0};
    if (param && psp_mem_ptr(param, 128)) {
        mode     = psp_read32(param + 48u);
        databuf  = psp_read32(param + 116u);
        bufsize  = psp_read32(param + 120u);
        datasize = psp_read32(param + 124u);
        savedata_path(path, sizeof path, param);
        if (mode == 0u || mode == 2u || mode == 4u) {          /* load */
            FILE *f = fopen(path, "rb");
            if (!f) result = 0x80110307u;                      /* no data */
            else {
                fseek(f, 0, SEEK_END);
                uint32_t n = (uint32_t)ftell(f);
                fseek(f, 0, SEEK_SET);
                if (n > bufsize) n = bufsize;
                void *dst = databuf ? psp_mem_ptr(databuf, n) : NULL;
                if (dst && n) { if (fread(dst, 1, n, f) != n) result = 0x80110308u; }
                fclose(f);
            }
        } else if (mode == 1u || mode == 3u || mode == 5u) {   /* save */
            uint32_t n = datasize ? datasize : bufsize;
            if (n > bufsize && bufsize) n = bufsize;
            const void *src = databuf ? psp_mem_ptr(databuf, n) : NULL;
            mkdir("sdmc:/djmax", 0777);
            mkdir("sdmc:/djmax/SAVE", 0777);
            FILE *f = fopen(path, "wb");
            if (!f || !src) result = 0x80110383u;              /* access error */
            else if (n && fwrite(src, 1, n, f) != n) result = 0x80110383u;
            if (f) fclose(f);
        }
        psp_write32(param + 28u, result);                      /* base.result */
    }
    psp_printf("[save] mode=%u %s -> 0x%08X (buf=0x%08X %u/%u B)\n",
               mode, path, result, databuf, datasize, bufsize);
    g_sd_status = 1; g_sd_calls = 0;
    psp_ret(0);
}
static void hle_UtilitySaveStatus(void) {
    if (!g_sd_status) { psp_ret(0); return; }
    g_sd_calls++;
    if (g_sd_calls <= 2) psp_ret(2);                           /* visible/running */
    else psp_ret(3);                                           /* terminado */
}
static void hle_UtilitySaveShutdown(void) { g_sd_status = 0; psp_ret(0); }

static void hle_UtilityMsgInit(void) {
    const uint32_t param = psp_arg(0);
    if (param && psp_mem_ptr(param, 32)) psp_write32(param + 28u, 0);
    g_md_status = 1; g_md_calls = 0;
    psp_ret(0);
}
static void hle_UtilityMsgStatus(void) {
    if (!g_md_status) { psp_ret(0); return; }
    g_md_calls++;
    if (g_md_calls <= 2) psp_ret(2);
    else psp_ret(3);
}
static void hle_UtilityMsgShutdown(void) { g_md_status = 0; psp_ret(0); }
static void hle_UtilityUpdate(void)      { psp_ret(0); }
static void hle_UtilityUnload(void)    { psp_ret(0); }
static void hle_RtcGetLocalTime(void) {
    const uint32_t t = psp_arg(0);
    if (t && psp_mem_ptr(t, 16)) for (uint32_t i = 0; i < 16u; i += 4u) psp_write32(t + i, 0);
    psp_ret(0);
}
/* ---- callbacks / GE / UMD / cache: the init values the threads expect ---- */
static uint32_t g_cb_uid = 0x00060000u;
static void hle_CreateCallback(void) {
    /* PS2 left this unimplemented, i.e. returned 0. Returning a valid uid made
     * the game create a second callback and diverge at NID 14; match PS2. */
    static uint32_t n;
    if (n++ < 2) psp_printf("[cb] CreateCallback -> 0 (PS2 semantics)\n");
    psp_ret(0);
}
static void hle_RegisterExitCallback(void) { psp_ret(0); }
static void hle_PowerRegisterCallback(void) { psp_ret(0); }
static void hle_GeEdramGetAddr(void) {
    /* PS2 left this at 0 (unimplemented) and the game still draws: it computes
     * its framebuffer as base+0x88000 and lets the GE own it. Matching PS2. */
    psp_ret(0);
}
static void hle_GeSetCallback(void) { psp_ret(0); }
static void hle_UmdActivate(void) { psp_ret(0); }
static void hle_DcacheInvalidate(void) { psp_ret(0); }
static uint32_t g_devctl_logged;
static void hle_IoDevctl(void) {
    if (g_devctl_logged < 6) { g_devctl_logged++;
        psp_printf("[io] devctl a0=0x%08X a1=0x%08X a2=0x%08X\n", psp_arg(0), psp_arg(1), psp_arg(2)); }
    psp_ret(0);
}
void ps2_hle_install(void) {
    ps2_audio_set_pump(ps2_sys_pump);   /* HOME responde incluso en esperas de audio */
    /* SysMem */
    psp_hle_register(0x237DBD4F, "SysMemUserForUser", "sceKernelAllocPartitionMemory", hle_AllocPartitionMemory);
    psp_hle_register(0xB6D61D02, "SysMemUserForUser", "sceKernelFreePartitionMemory",  hle_FreePartitionMemory);
    psp_hle_register(0x9D9A5BA1, "SysMemUserForUser", "sceKernelGetBlockHeadAddr",     hle_GetBlockHeadAddr);
    psp_hle_register(0xA291F107, "SysMemUserForUser", "sceKernelMaxFreeMemSize",       hle_MaxFreeMemSize);
    psp_hle_register(0x7591C7DB, "SysMemUserForUser", "sceKernelSetCompiledSdkVersion",hle_Ok0);
    psp_hle_register(0xF77D77CB, "SysMemUserForUser", "sceKernelSetCompilerVersion",   hle_Ok0);
    psp_hle_register(0x13A5ABEF, "SysMemUserForUser", "sceKernelPrintf",               hle_Printf);
    psp_hle_register(0xF919F628, "SysMemUserForUser", "sceKernelTotalFreeMemSize",      hle_TotalFreeMemSize);
    psp_hle_register(0x092968F4, "Kernel_Library",    "sceKernelCpuSuspendIntr",       hle_SuspendIntr);
    psp_hle_register(0x5F10D406, "Kernel_Library",    "sceKernelCpuResumeIntr",        hle_ResumeIntr);
    psp_hle_register_unnamed(0x91DE343C, "SysMemUserForUser",                          hle_Ok0);

    /* ThreadMan */
    psp_hle_register(0x446D8DE6, "ThreadManForUser", "sceKernelCreateThread",     hle_CreateThread);
    psp_hle_register(0xF475845D, "ThreadManForUser", "sceKernelStartThread",      hle_StartThread);
    psp_hle_register(0xAA73C935, "ThreadManForUser", "sceKernelExitThread",       hle_ExitThread);
    psp_hle_register(0x809CE29B, "ThreadManForUser", "sceKernelExitDeleteThread", hle_ExitThread);
    psp_hle_register(0x9FA03CD3, "ThreadManForUser", "sceKernelDeleteThread",     hle_DeleteThread);
    psp_hle_register(0x278C0DF5, "ThreadManForUser", "sceKernelWaitThreadEnd",    hle_WaitThreadEnd);
    psp_hle_register(0x840E8133, "ThreadManForUser", "sceKernelWaitThreadEndCB",  hle_WaitThreadEnd);
    psp_hle_register(0xCEADEB47, "ThreadManForUser", "sceKernelDelayThread",      hle_DelayThread);
    psp_hle_register(0x68DA9E36, "ThreadManForUser", "sceKernelDelayThreadCB",    hle_DelayThread);
    psp_hle_register(0x9ACE131E, "ThreadManForUser", "sceKernelSleepThread",      hle_SleepThread);
    psp_hle_register(0x82826F70, "ThreadManForUser", "sceKernelSleepThreadCB",    hle_SleepThread);
    psp_hle_register(0x293B45B8, "ThreadManForUser", "sceKernelGetThreadId",      hle_GetThreadId);
    psp_hle_register(0x3B183E26, "ThreadManForUser", "sceKernelGetThreadExitStatus", hle_GetThreadExitStatus);
    psp_hle_register(0x82BC5777, "ThreadManForUser", "sceKernelGetSystemTimeWide",hle_GetSystemTimeWide);
    psp_hle_register(0xDB738F35, "ThreadManForUser", "sceKernelGetSystemTime",    hle_GetSystemTime);
    psp_hle_register(0x369ED59D, "ThreadManForUser", "sceKernelGetSystemTimeLow", hle_GetSystemTimeLow);
    psp_hle_register(0xBA6B92E2, "ThreadManForUser", "sceKernelSysClock2USec",    hle_SysClock2USec);
    psp_hle_register(0x52089CA1, "ThreadManForUser", "sceKernelGetThreadStackFreeSize", hle_Ok0);
    psp_hle_register(0x71BC9871, "ThreadManForUser", "sceKernelChangeThreadPriority", hle_ChangeThreadPriority);
    psp_hle_register(0xD6DA4BA1, "ThreadManForUser", "sceKernelCreateSema",       hle_CreateSema);
    psp_hle_register(0x28B6489C, "ThreadManForUser", "sceKernelDeleteSema",       hle_DeleteSema);
    psp_hle_register(0x3F53E640, "ThreadManForUser", "sceKernelSignalSema",       hle_SignalSema);
    psp_hle_register(0x4E3A1105, "ThreadManForUser", "sceKernelWaitSema",         hle_WaitSema);
    psp_hle_register(0x58B1F937, "ThreadManForUser", "sceKernelPollSema",         hle_PollSema);
    psp_hle_register(0x55C20A00, "ThreadManForUser", "sceKernelCreateEventFlag",  hle_CreateEventFlag);
    psp_hle_register(0xEF9E4C70, "ThreadManForUser", "sceKernelDeleteEventFlag",  hle_DeleteEventFlag);
    psp_hle_register(0x1FB15A32, "ThreadManForUser", "sceKernelSetEventFlag",     hle_SetEventFlag);
    psp_hle_register(0x812346E4, "ThreadManForUser", "sceKernelClearEventFlag",   hle_ClearEventFlag);
    psp_hle_register(0x402FCF22, "ThreadManForUser", "sceKernelWaitEventFlag",    hle_WaitEventFlag);
    psp_hle_register(0x30FD48F0, "ThreadManForUser", "sceKernelPollEventFlag",    hle_PollEventFlag);
    psp_hle_register(0xA66B0120, "ThreadManForUser", "sceKernelReferEventFlagStatus", hle_ReferEventFlagStatus);
    psp_hle_register(0x56C039B5, "ThreadManForUser", "sceKernelCreateVpl",       hle_CreateVpl);
    psp_hle_register(0x89B3D48C, "ThreadManForUser", "sceKernelDeleteVpl",       hle_DeleteVpl);
    psp_hle_register(0xBED27435, "ThreadManForUser", "sceKernelAllocateVpl",     hle_AllocateVpl);
    psp_hle_register(0xEC0A693F, "ThreadManForUser", "sceKernelAllocateVplCB",   hle_AllocateVpl);
    psp_hle_register(0xAF36D708, "ThreadManForUser", "sceKernelTryAllocateVpl",  hle_AllocateVpl);
    psp_hle_register(0xB736E9FF, "ThreadManForUser", "sceKernelFreeVpl",         hle_FreeVpl);

    /* IoFileMgr: disc0: from host:, an empty ms0. */
    psp_hle_register(0x109F50BC, "IoFileMgrForUser", "sceIoOpen",      hle_IoOpenPath);
    psp_hle_register(0x89AA9906, "IoFileMgrForUser", "sceIoOpenAsync", hle_IoOpenAsync2);
    psp_hle_register(0x810C4BC3, "IoFileMgrForUser", "sceIoClose",     hle_IoClose);
    psp_hle_register(0x6A638D83, "IoFileMgrForUser", "sceIoRead",      hle_IoRead);
    psp_hle_register(0xA0B5A7C2, "IoFileMgrForUser", "sceIoReadAsync", hle_IoReadAsync);
    psp_hle_register(0x42EC03AC, "IoFileMgrForUser", "sceIoWrite",     hle_IoWrite);
    psp_hle_register(0x172D316E, "StdioForUser", "sceKernelStdin",  hle_KernelStdin);
    psp_hle_register(0x617F3FE6, "sceDmac", "sceDmacMemcpy",  hle_DmacMemcpy);
    psp_hle_register(0x0BF0A3AE, "sceNet", "sceNetGetLocalEtherAddr",            hle_NetGetLocalEtherAddr);
    psp_hle_register(0xDFE53E03, "sceNetAdhoc", "sceNetAdhocPdpRecv",            hle_AdhocPdpRecv);
    psp_hle_register(0xABED3790, "sceNetAdhoc", "sceNetAdhocPdpSend",            hle_AdhocPdpSend);
    psp_hle_register(0x75ECD386, "sceNetAdhocctl", "sceNetAdhocctlGetState",     hle_AdhocctlGetState);
    psp_hle_register(0xE162CB14, "sceNetAdhocctl", "sceNetAdhocctlGetPeerList",  hle_AdhocctlGetPeerList);
    psp_hle_register(0xC58BCD9E, "sceNetAdhocMatching", "sceNetAdhocMatchingGetMembers", hle_AdhocMatchingGetMembers);
    psp_hle_register(0x4DB1E739, "sceUtility", "sceUtilityNetconfInitStart",     hle_NetconfInitStart);
    psp_hle_register(0x6332AA39, "sceUtility", "sceUtilityNetconfGetStatus",     hle_NetconfGetStatus);
    psp_hle_register(0x91E70E35, "sceUtility", "sceUtilityNetconfUpdate",        hle_NetconfUpdate);
    psp_hle_register(0xF88155F6, "sceUtility", "sceUtilityNetconfShutdownStart", hle_NetconfShutdownStart);
    psp_hle_register(0xA6BAB2E9, "StdioForUser", "sceKernelStdout", hle_KernelStdout);
    psp_hle_register(0xF78BA90A, "StdioForUser", "sceKernelStderr", hle_KernelStderr);
    psp_hle_register(0x35DBD746, "IoFileMgrForUser", "sceIoWaitAsyncCB", hle_IoWaitAsync);
    psp_hle_register(0xE23EEC33, "IoFileMgrForUser", "sceIoWaitAsync", hle_IoWaitAsync);
    psp_hle_register(0x3251EA56, "IoFileMgrForUser", "sceIoPollAsync", hle_IoPollAsync);
    psp_hle_register(0xCB05F8D6, "IoFileMgrForUser", "sceIoGetAsyncStat", hle_IoPollAsync);
    psp_hle_register(0xFF5940B6, "IoFileMgrForUser", "sceIoCloseAsync", hle_IoCloseAsync);
    psp_hle_register(0xA12A0514, "IoFileMgrForUser", "sceIoSetAsyncCallback", hle_IoSetAsyncCallback);
    psp_hle_register(0x27EB27B8, "IoFileMgrForUser", "sceIoLseek",     hle_IoLseek);
    psp_hle_register(0x71B19E77, "IoFileMgrForUser", "sceIoLseekAsync", hle_IoLseekAsync);
    psp_hle_register(0xB29DDF9C, "IoFileMgrForUser", "sceIoDopen",     hle_IoDopen);
    psp_hle_register(0xE3EB004C, "IoFileMgrForUser", "sceIoDread",     hle_IoReturn0);
    psp_hle_register(0xEB092469, "IoFileMgrForUser", "sceIoDclose",    hle_IoClose);
    psp_hle_register(0xACE946E8, "IoFileMgrForUser", "sceIoGetstat",   hle_IoGetstat);
    psp_hle_register(0xF27A9C51, "IoFileMgrForUser", "sceIoRemove",    hle_IoReturnNeg);
    psp_hle_register(0x06A70004, "IoFileMgrForUser", "sceIoMkdir",     hle_IoReturnNeg);
    psp_hle_register(0x779103A0, "IoFileMgrForUser", "sceIoRename",    hle_IoReturnNeg);

    psp_hle_register(0x05572A5F, "LoadExecForUser", "sceKernelExitGame", hle_ExitGame);

    /* UMD: the game polls the drive before it draws anything. */
    psp_hle_register(0x6B4A146C, "sceUmdUser", "sceUmdGetDriveStat", hle_UmdDriveStat);
    psp_hle_register(0x8EF08FCE, "sceUmdUser", "sceUmdWaitDriveStat", hle_IoReturn0);
    psp_hle_register(0x5D268707, "sceAtrac3plus", "sceAtrac (unnamed)", hle_IoReturn0);
    psp_hle_register(0x56202973, "sceUmdUser", "sceUmdWaitDriveStat", hle_UmdWaitStat);
    psp_hle_register(0x56202973, "sceUmdUser", "sceUmdWaitDriveStatCB", hle_UmdWaitStat);

    /* GE + display details the game reaches before its first frame. */
    psp_hle_register(0xAB49E76A, "sceGe_user", "sceGeListEnQueue",      hle_GeEnqueue);
    psp_hle_register(0x289D82FE, "sceDisplay", "sceDisplaySetFrameBuf", hle_SetFrameBuf);
    psp_hle_register(0x9C6EAAD7, "sceDisplay", "sceDisplayGetVcount",   hle_GetVcount);
    psp_hle_register(0x984C27E7, "sceDisplay", "sceDisplayWaitVblankStart",   hle_WaitVblank);
    psp_hle_register(0x46F186C3, "sceDisplay", "sceDisplayWaitVblankStartCB", hle_WaitVblank);
    psp_hle_register(0xDBA6C4C4, "sceDisplay", "sceDisplayGetFramePerSec",    hle_GetFramePerSec);
    psp_hle_register(0x1F803938, "sceCtrl", "sceCtrlReadBufferPositive", hle_CtrlRead);
    psp_hle_register(0x2BCEC83E, "sceHprm", "sceHprmPeekLatch",          hle_HprmLatch);
    psp_hle_register(0x40D2F9F0, "sceHprm", "sceHprmReadLatch",          hle_HprmLatch);
    psp_hle_register(0x1910B327, "sceHprm", "sceHprmPeekCurrentKey",     hle_HprmCurrentKey);
    psp_hle_register(0x208DB1BD, "sceHprm", "sceHprmIsRemoteExist",      hle_HprmNone);
    psp_hle_register(0x7E69EDA4, "sceHprm", "sceHprmIsHeadphoneExist",   hle_HprmNone);
    psp_hle_register(0x219C58F1, "sceHprm", "sceHprmIsMicrophoneExist",  hle_HprmNone);
    psp_hle_register(0x136CAF51, "sceAudio", "sceAudioOutputBlocking",        hle_AudioOutputBlocking);
    psp_hle_register(0x8C1009B2, "sceAudio", "sceAudioOutput",                hle_AudioOutput);
    psp_hle_register(0x13F592BC, "sceAudio", "sceAudioOutputPannedBlocking",  hle_AudioOutputPannedBlocking);
    psp_hle_register(0xE2D56B2D, "sceAudio", "sceAudioOutputPanned",          hle_AudioOutputPanned);
    psp_hle_register(0x5EC81C55, "sceAudio", "sceAudioChReserve",             hle_AudioChReserve);
    psp_hle_register(0x6FC46853, "sceAudio", "sceAudioChRelease",             hle_AudioChRelease);
    psp_hle_register(0xB011922F, "sceAudio", "sceAudioGetChannelRestLength",  hle_AudioRestLen);
    psp_hle_register(0x2D53F36E, "sceAudio", "sceAudioGetChannelRestLen",     hle_AudioRestLen);
    psp_hle_register(0x95FD0C2D, "sceAudio", "sceAudioChangeChannelConfig",   hle_IoReturn0);
    psp_hle_register(0xB7E1D8E7, "sceAudio", "sceAudioChangeChannelVolume",   hle_IoReturn0);
    psp_hle_register(0xCB2E439E, "sceAudio", "sceAudioSetChannelDataLen",     hle_AudioSetChannelDataLen);
    psp_hle_register(0x1F4011E6, "sceCtrl", "sceCtrlSetSamplingMode",    hle_IoReturn0);
    psp_hle_register(0x6A2774F3, "sceCtrl", "sceCtrlSetSamplingCycle",   hle_IoReturn0);

    /* sceMpeg: coherent objects, but the stream ends at the first AU so the
     * player closes the opening movie instead of waiting forever. */
    psp_hle_register(0x682A619B, "sceMpeg", "sceMpegInit",               hle_MpegInitOk);
    psp_hle_register(0x874624D6, "sceMpeg", "sceMpegFinish",             hle_IoReturn0);
    psp_hle_register(0xC132E22F, "sceMpeg", "sceMpegQueryMemSize",       hle_MpegQueryMemSize);
    psp_hle_register(0xD7A29F46, "sceMpeg", "sceMpegRingbufferQueryMemSize", hle_MpegQueryMemSize);
    psp_hle_register(0x37295ED8, "sceMpeg", "sceMpegRingbufferConstruct", hle_MpegRingbufConstruct);
    psp_hle_register(0xB240A59E, "sceMpeg", "sceMpegRingbufferPut",      hle_MpegRingbufPut);
    psp_hle_register(0xB5F6DC87, "sceMpeg", "sceMpegRingbufferAvailableSize", hle_MpegRingbufAvailable);
    psp_hle_register(0x13407F13, "sceMpeg", "sceMpegRingbufferDestruct", hle_IoReturn0);
    psp_hle_register(0xD8C5F121, "sceMpeg", "sceMpegCreate",             hle_MpegCreate);
    psp_hle_register(0x606A4649, "sceMpeg", "sceMpegDelete",             hle_IoReturn0);
    psp_hle_register(0x42560F23, "sceMpeg", "sceMpegRegistStream",       hle_MpegRegistStream);
    psp_hle_register(0x591A4AA2, "sceMpeg", "sceMpegUnRegistStream",     hle_IoReturn0);
    psp_hle_register(0x21FF80E4, "sceMpeg", "sceMpegQueryStreamOffset",  hle_IoReturn0);
    psp_hle_register(0x611E9E11, "sceMpeg", "sceMpegQueryStreamSize",    hle_IoReturn0);
    psp_hle_register(0x167AFD9E, "sceMpeg", "sceMpegInitAu",             hle_IoReturn0);
    psp_hle_register(0xA780CF7E, "sceMpeg", "sceMpegMallocAvcEsBuf",     hle_MpegEsBuf);
    psp_hle_register(0xCEB870B1, "sceMpeg", "sceMpegFreeAvcEsBuf",       hle_IoReturn0);
    psp_hle_register(0x0E3C2E9D, "sceMpeg", "sceMpegAvcDecode",          hle_MpegAvcDecode);
    psp_hle_register(0x740FCCD1, "sceMpeg", "sceMpegAvcDecodeStop",      hle_IoReturn0);
    psp_hle_register(0x4571CC64, "sceMpeg", "sceMpegAvcDecodeFlush",     hle_IoReturn0);
    psp_hle_register(0x800C44DF, "sceMpeg", "sceMpegAtracDecode",        hle_MpegAtrac);
    psp_hle_register(0xFE246728, "sceMpeg", "sceMpegGetAvcAu",           hle_MpegGetAvcAu);
    psp_hle_register(0xE1CE83A7, "sceMpeg", "sceMpegGetAtracAu",         hle_MpegAtrac);
    psp_hle_register(0xF8DCB679, "sceMpeg", "sceMpegQueryAtracEsSize",   hle_MpegQueryAtracEsSize);
    psp_hle_register(0x349D6D6C, "ThreadManForUser", "sceKernelCheckCallback", hle_IoReturn0);
    psp_hle_register(0x090CCB3F, "sceSuspendForUser", "sceKernelPowerTick",    hle_IoReturn0);

    /* GE */
    psp_hle_register(0xB448EC0D, "sceGe_user", "sceGeBreak",               hle_IoReturn0);
    psp_hle_register(0x1C0D95A6, "sceGe_user", "sceGeListEnQueueHead",      hle_GeEnqueue);
    psp_hle_register(0xE0D68148, "sceGe_user", "sceGeListUpdateStallAddr",  hle_GeUpdateStall);
    psp_hle_register(0x4C06E472, "sceGe_user", "sceGeContinue",             hle_GeSync);
    psp_hle_register(0x03444EB4, "sceGe_user", "sceGeListSync",             hle_GeSync);
    psp_hle_register(0xB287BD61, "sceGe_user", "sceGeDrawSync",             hle_GeSync);

    /* Display / utility / rtc: no-ops that count. */
    psp_hle_register(0x0E20F177, "sceDisplay", "sceDisplaySetMode",         hle_GeSync);
    psp_hle_register(0x36CDFADE, "sceDisplay", "sceDisplayWaitVblankStart", hle_GeSync);
    psp_hle_register(0x984C27E7, "sceDisplay", "sceDisplayWaitVblank",      hle_GeSync);

    /* SAS audio core: the real NIDs are the __sceSas* ones from
     * analysis/psp_imports.csv; the block above used PS2-era guesses. */
    psp_hle_register(0x42778A9F, "sceSasCore", "__sceSasInit",              hle_SasInit);
    psp_hle_register(0x927AC2DA, "sceSasCore", "sceSasInit",                hle_SasInit);
    psp_hle_register(0xA3589D81, "sceSasCore", "__sceSasCore",              hle_SasCore);
    psp_hle_register(0xE175EF66, "sceSasCore", "sceSasCore",                hle_SasCore);
    psp_hle_register(0x50A14DFC, "sceSasCore", "__sceSasCoreWithMix",       hle_SasCoreWithMix);
    psp_hle_register(0x99944089, "sceSasCore", "__sceSasSetVoice",          hle_SasSetVoice);
    psp_hle_register(0xAD84D37F, "sceSasCore", "__sceSasSetPitch",          hle_SasSetPitch);
    psp_hle_register(0x440CA7D8, "sceSasCore", "__sceSasSetVolume",         hle_SasSetVolume);
    psp_hle_register(0x019B25EB, "sceSasCore", "__sceSasSetADSR",           hle_SasOk);
    psp_hle_register(0x9EC3676A, "sceSasCore", "__sceSasSetADSRmode",       hle_SasOk);
    psp_hle_register(0xCBCD4F79, "sceSasCore", "__sceSasSetSimpleADSR",     hle_SasOk);
    psp_hle_register(0x5F9529F6, "sceSasCore", "__sceSasSetSL",             hle_SasOk);
    psp_hle_register(0xB7660A23, "sceSasCore", "__sceSasSetNoise",          hle_SasOk);
    psp_hle_register(0x787D04D5, "sceSasCore", "__sceSasSetPause",          hle_SasOk);
    psp_hle_register(0x76F01ACA, "sceSasCore", "__sceSasSetKeyOn",          hle_SasKeyOn);
    psp_hle_register(0xA0CF2FA4, "sceSasCore", "__sceSasSetKeyOff",         hle_SasKeyOff);
    psp_hle_register(0x267A6DD2, "sceSasCore", "__sceSasRevParam",          hle_SasRevParam);
    psp_hle_register(0x33D4AB37, "sceSasCore", "__sceSasRevType",           hle_SasRevType);
    psp_hle_register(0xD5A229C9, "sceSasCore", "__sceSasRevEVOL",           hle_SasRevEVOL);
    psp_hle_register(0xF983B186, "sceSasCore", "__sceSasRevVON",            hle_SasRevVON);
    psp_hle_register(0x68A46B95, "sceSasCore", "__sceSasGetEndFlag",        hle_SasGetEndFlag);
    psp_hle_register(0x2C8E6AB3, "sceSasCore", "__sceSasGetPauseFlag",      hle_SasGetZero);
    psp_hle_register(0x74AE582A, "sceSasCore", "__sceSasGetEnvelopeHeight", hle_SasGetEnvelopeHeight);

    /* utils / callbacks / GE / UMD */
    psp_hle_register(0xB435DEC5, "UtilsForUser", "sceKernelDcacheWritebackInvalidateAll", hle_DcacheInvalidate);
    psp_hle_register(0xE81CAF8F, "ThreadManForUser", "sceKernelCreateCallback", hle_CreateCallback);
    psp_hle_register(0x4AC57943, "LoadExecForUser", "sceKernelRegisterExitCallback", hle_RegisterExitCallback);
    psp_hle_register(0x04B7766E, "scePower", "scePowerRegisterCallback", hle_PowerRegisterCallback);
    psp_hle_register(0xE47E40E4, "sceGe_user", "sceGeEdramGetAddr", hle_GeEdramGetAddr);
    psp_hle_register(0xA4FC06A4, "sceGe_user", "sceGeSetCallback", hle_GeSetCallback);
    psp_hle_register(0xC6183D47, "sceUmdUser", "sceUmdActivate", hle_UmdActivate);
    psp_hle_register(0x54F5FB11, "IoFileMgrForUser", "sceIoDevctl", hle_IoDevctl);

    /* the five NIDs left in the movie/audio init loop */
    psp_hle_register(0xA5DA2406, "sceUtility", "sceUtilityGetSystemParamInt", hle_UtilityGetSystemParamInt);
    psp_hle_register(0x36AA6E91, "sceImpose", "sceImposeSetLanguageMode", hle_ImposeSetLanguageMode);
    psp_hle_register(0x2A2B3DE0, "sceUtility", "sceUtilityLoadModule", hle_UtilityLoadModule);
    psp_hle_register(0x05DB22CE, "sceGe_user", "sceGeUnsetCallback", hle_GeUnsetCallback);
    psp_hle_register(0xEBD177D6, "scePower", "scePowerGetBatteryChargeStatus", hle_PowerUnknown);

    /* Leftovers from the first-frame loop (calls counted in analysis/3ds-port.md §11.3):
     * the 4616-call one is just the data-cache writeback, once per frame. */
    psp_hle_register(0x79D1C3FA, "UtilsForUser", "sceKernelDcacheWritebackAll", hle_DcacheInvalidate);
    psp_hle_register(0xD59EAD2F, "ThreadManForUser", "sceKernelWakeupThread", hle_IoReturn0);
    psp_hle_register(0x383F7BCC, "ThreadManForUser", "sceKernelWakeupThread?", hle_IoReturn0);

    /* identified by SHA-1: Atrac3plus (real, atrac_3ds.c), Utility dialogs and RTC */
    ps2_atrac_install();
    psp_hle_register(0x2AD8E239, "sceUtility", "sceUtilityMsgDialogInitStart", hle_UtilityMsgInit);
    psp_hle_register(0x9A1C91D7, "sceUtility", "sceUtilityMsgDialogGetStatus", hle_UtilityMsgStatus);
    psp_hle_register(0x95FC253B, "sceUtility", "sceUtilityMsgDialogUpdate",    hle_UtilityUpdate);
    psp_hle_register(0x67AF3428, "sceUtility", "sceUtilityMsgDialogShutdownStart", hle_UtilityMsgShutdown);
    psp_hle_register(0x4928BD96, "sceUtility", "sceUtilityMsgDialogAbort",     hle_UtilityMsgShutdown);
    psp_hle_register(0x50C4CD57, "sceUtility", "sceUtilitySavedataInitStart",  hle_UtilitySaveInit);
    psp_hle_register(0x8874DBE0, "sceUtility", "sceUtilitySavedataGetStatus",  hle_UtilitySaveStatus);
    psp_hle_register(0xD4B95FFB, "sceUtility", "sceUtilitySavedataUpdate",     hle_UtilityUpdate);
    psp_hle_register(0x9790B33C, "sceUtility", "sceUtilitySavedataShutdownStart", hle_UtilitySaveShutdown);
    psp_hle_register(0xE49BFE92, "sceUtility", "sceUtilityUnloadModule",       hle_UtilityUnload);
    psp_hle_register(0xE7C27D1B, "sceRtc", "sceRtcGetCurrentClockLocalTime",   hle_RtcGetLocalTime);
}
