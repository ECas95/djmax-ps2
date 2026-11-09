/* ps2_sched.c -- cooperative guest-thread scheduler on the EE.
 *
 * See ps2_sched.h for the model. Everything here is deterministic: FIFO round
 * robin, switches only at the firmware calls the HLE treats as blocking.
 */
#include "ps2_sched.h"
#include "runtime_min.h"
#include "ps2_hle.h"

#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

enum { TS_FREE = 0, TS_DORMANT, TS_READY, TS_RUNNING, TS_BLOCKED, TS_DEAD };

typedef struct {
    int      used;
    int      state;
    int      started;
    uint32_t uid;
    uint32_t entry;
    uint32_t arg, arglen, prio;
    uint32_t gstack, gstack_size;
    uint32_t exit_status;
    psp_cpu_state cpu;
    jmp_buf  ctx;
    uint8_t *estack;
    uint32_t estack_size;
    uint32_t top;
    uint32_t sp;
    uint32_t stack_used_max;
    uint32_t wait_ef;
    uint32_t switches;
    uint32_t fcyc, fsw, fmax, ffast;   /* TEMP [slow]: this frame's cycles, slices, longest, no-op yields */
    char     name[32];
} ps2_thread;

static ps2_thread g_th[PS2_THREAD_MAX];
/* The C stacks of the guest threads live in guest RAM, in the reserved stack area
 * (ps2_stack_alloc, outside every heap the game sees): the ELF has no room for 384 KB. */
static uint8_t   *g_ee_stacks;

static uint8_t *ee_stack(int i) {
    if (!g_ee_stacks) {
        const uint32_t n = PS2_THREAD_MAX * PS2_EE_STACK_SIZE;
        const uint32_t g = ps2_stack_alloc(n);
        g_ee_stacks = g ? (uint8_t *)psp_mem_ptr(g, n) : NULL;
        psp_printf("[sched] EE stacks at guest 0x%08X (%u KB)" "\n", g, n / 1024u);
        if (!g_ee_stacks) return NULL;
    }
    return g_ee_stacks + (uint32_t)i * PS2_EE_STACK_SIZE;
}
static int        g_cur = -1;
static int        g_created;
static uint32_t   g_next_uid = 0x00040000u;
static jmp_buf    g_sched_ctx;

static inline uint32_t cop0_count(void) { uint32_t c; __asm__ volatile("mfc0 %0, $9" : "=r"(c)); return c; }
static uint32_t g_slice_t0;
uint32_t ps2_sched_slice_cycles(void) { return cop0_count() - g_slice_t0; }

static void slice_end(void) {
    if (g_cur < 0) return;
    ps2_thread *t = &g_th[g_cur];
    const uint32_t d = cop0_count() - g_slice_t0;
    t->fcyc += d; t->fsw++;
    if (d > t->fmax) t->fmax = d;
}

/* Called with a fresh EE stack ($sp = a1). */
extern void ee_run_on_stack(void (*fn)(void), uint32_t sp);

__asm__(
".set push\n"
".set noreorder\n"
".text\n"
".globl ee_run_on_stack\n"
".type ee_run_on_stack,@function\n"
"ee_run_on_stack:\n"
"    addiu $sp, $sp, -32\n"
"    sw    $ra, 28($sp)\n"
"    sw    $s0, 24($sp)\n"
"    sw    $s1, 20($sp)\n"
"    move  $s0, $sp\n"           /* original EE sp */
"    move  $s1, $a0\n"           /* function */
"    move  $sp, $a1\n"           /* thread stack */
"    jalr  $s1\n"
"    nop\n"
"    move  $sp, $s0\n"
"    lw    $s1, 20($sp)\n"
"    lw    $s0, 24($sp)\n"
"    lw    $ra, 28($sp)\n"
"    addiu $sp, $sp, 32\n"
"    jr    $ra\n"
"    nop\n"
".size ee_run_on_stack, .-ee_run_on_stack\n"
".set pop\n");

static void thread_trampoline(void) {
    ps2_thread *t = &g_th[g_cur];
    psp_printf("sched: trampoline uid=0x%08X entry=0x%08X\n", t->uid, t->entry);
    psp_cpu = t->cpu;

    psp_call_from_host(t->entry);

    t->exit_status = psp_cpu.r[PSP_REG_V0];
    t->state = TS_DEAD;
    longjmp(g_sched_ctx, 1);      /* back to ps2_sched_pump */
}

void ps2_sched_init(void) {
    memset(g_th, 0, sizeof g_th);
    g_cur = -1;
    g_created = 0;
}

int ps2_sched_create(uint32_t entry, const char *name, uint32_t prio,
                     uint32_t stack_size, uint32_t arglen, uint32_t arg,
                     uint32_t *uid) {
    for (int i = 0; i < PS2_THREAD_MAX; i++) {
        ps2_thread *t = &g_th[i];
        if (t->used && t->state != TS_DEAD) continue;

        /* A dead thread's slot is reused, and its guest stack if it is big enough. */
        const uint32_t old_gstack = t->used ? t->gstack : 0;
        const uint32_t old_size = t->used ? t->gstack_size : 0;
        memset(t, 0, sizeof *t);
        t->used = 1;
        t->state = TS_DORMANT;
        t->uid = g_next_uid++;
        t->entry = entry;
        t->prio = prio;
        t->arglen = arglen;
        t->arg = arg;
        /* Guest stack from the guest pool (17.6 MB), never from the ~300 KB EE
         * heap: that is what the firmware does and what the game expects. A
         * tiny/zero request is rounded up, not rejected. */
        t->gstack_size = stack_size < 0x1000u ? 0x4000u : stack_size;
        t->gstack = t->gstack_size <= old_size ? old_gstack
                  : ps2_stack_alloc(t->gstack_size);   /* reserved top area */
        if (!t->gstack) {
            psp_printf("[sched] CreateThread: no guest stack (size=%u) uid=0x%X\n",
                       t->gstack_size, t->uid);
            t->used = 0;
            return -1;
        }

        t->estack_size = PS2_EE_STACK_SIZE;
        t->estack = ee_stack(i);
        t->top = ((uint32_t)t->estack + t->estack_size) & ~15u;
        *(volatile uint32_t *)t->estack = 0xDEADBEEFu; /* canary at the bottom */
        t->stack_used_max = 0;
        psp_printf("[sched] create uid=0x%X size=%u gbase=0x%08X gend=0x%08X "
                   "estack=0x%08X top=0x%08X\n",
                   t->uid, t->gstack_size, t->gstack, t->gstack + t->gstack_size,
                   (uint32_t)t->estack, t->top);

        snprintf(t->name, sizeof t->name, "%s", name ? name : "?");
        g_created++;
        *uid = t->uid;
        return 0;
    }
    return -1;
}

static int find(uint32_t uid) {
    for (int i = 0; i < PS2_THREAD_MAX; i++)
        if (g_th[i].used && g_th[i].uid == uid) return i;
    return -1;
}

int ps2_sched_start(uint32_t uid, uint32_t arglen, uint32_t arg) {
    const int i = find(uid);
    if (i < 0) return -1;
    ps2_thread *t = &g_th[i];

    t->arglen = arglen;
    t->arg = arg;
    t->cpu = psp_cpu;
    /* PSP ABI: entry(SceSize args, void *argp); argp is a copy on the new stack. */
    uint32_t sp = (t->gstack + t->gstack_size - 64u) & ~15u;
    uint32_t argp = 0;
    if (arg && arglen) {
        sp = (sp - arglen) & ~15u;
        argp = sp;
        for (uint32_t b = 0; b < arglen; b++) psp_write8(argp + b, psp_read8(arg + b));
    }
    t->cpu.r[PSP_REG_A0] = arglen;
    t->cpu.r[PSP_REG_A1] = argp;
    t->cpu.r[PSP_REG_SP] = sp;
    psp_printf("[sched] start uid=0x%X sp=0x%08X size=%u gbase=0x%08X\n",
               t->uid, t->cpu.r[PSP_REG_SP], t->gstack_size, t->gstack);
    t->cpu.r[PSP_REG_RA] = 0;
    t->cpu.frame = NULL;
    t->cpu.unwind = 0;
    t->state = TS_READY;
    t->started = 0;
    return 0;
}

static int first_ready_except(int except) {
    for (int k = 1; k <= PS2_THREAD_MAX; k++) {
        const int base = (except < 0) ? 0 : except;
        const int j = (base + k) % PS2_THREAD_MAX;
        if (g_th[j].used && g_th[j].state == TS_READY) return j;
    }
    return -1;
}

/* Each thread owns its C stack, so nothing needs copying: just jump to the scheduler. */
static void switch_out(int new_state) {
    ps2_thread *me = &g_th[g_cur];
    volatile uint32_t marker = 0;
    uint32_t sp = ((uint32_t)&marker) & ~15u;

    if (*(volatile uint32_t *)me->estack != 0xDEADBEEFu) {
        psp_printf("[sched] stack overflow uid=0x%X (canary=0x%08X) top=0x%08X sp=0x%08X\n",
                   me->uid, *(volatile uint32_t *)me->estack, me->top, sp);
        me->state = TS_DEAD;
        longjmp(g_sched_ctx, 1);
    }
    const uint32_t used = me->top - sp;
    if (used > me->stack_used_max) me->stack_used_max = used;
    me->sp = sp;
    me->cpu = psp_cpu;
    { static uint32_t sw; if (sw < 8) { sw++;
        psp_printf("[sched] switch uid=0x%X sp=0x%08X top=0x%08X\n", me->uid, sp, me->top); } }

    if (setjmp(me->ctx) == 0) {
        me->state = new_state;
        longjmp(g_sched_ctx, 1);
    }
    /* resumed: psp_cpu has been restored by the scheduler */
}

static int other_ready(void) {
    for (int i = 0; i < PS2_THREAD_MAX; i++)
        if (i != g_cur && g_th[i].used && g_th[i].state == TS_READY) return 1;
    return 0;
}

/* Nobody else to run: the scheduler would pick this thread again, after copying its
 * state out and back in (1.6 KB) -- the cost of every turn of a wait loop. */
void ps2_sched_yield(void) {
    if (g_cur < 0) return;
    if (!other_ready()) { g_th[g_cur].ffast++; return; }
    switch_out(TS_READY);
}

/* PSP priorities (lower number = more urgent) only matter here at the two calls that
 * hand the CPU over at once on a PSP: starting a thread more urgent than the caller, and
 * the caller making itself less urgent than a ready thread. The movie player depends on
 * it: its thread clears a "stopped" flag first thing, before the main loop reads it. */
int ps2_sched_set_prio(uint32_t uid, uint32_t prio) {
    const int i = uid ? find(uid) : g_cur;
    if (i < 0) return -1;
    g_th[i].prio = prio;
    return 0;
}

void ps2_sched_preempt(void) {
    if (g_cur < 0) return;
    for (int i = 0; i < PS2_THREAD_MAX; i++)
        if (i != g_cur && g_th[i].used && g_th[i].state == TS_READY && g_th[i].prio < g_th[g_cur].prio) {
            switch_out(TS_READY);
            return;
        }
}

/* TEMP: per-thread cycles of a frame that missed its vblank (called by WaitVblank). */
extern uint32_t g_ge_cyc;
void ps2_sched_frame(uint32_t vblanks) {
    static uint32_t nprint;
    slice_end();
    g_slice_t0 = cop0_count();
    if (vblanks > 1u && nprint < 600u) {
        nprint++;
        psp_printf("[slow] vbl=%u ge=%uk", vblanks, g_ge_cyc / 1000u);
        for (int i = 0; i < PS2_THREAD_MAX; i++)
            if (g_th[i].used && (g_th[i].fsw || g_th[i].ffast))
                psp_printf(" %s=%uk/%u/max%uk/f%u", g_th[i].name, g_th[i].fcyc / 1000u, g_th[i].fsw,
                           g_th[i].fmax / 1000u, g_th[i].ffast);
        psp_printf("\n");
    }
    g_ge_cyc = 0;
    for (int i = 0; i < PS2_THREAD_MAX; i++) g_th[i].fcyc = g_th[i].fsw = g_th[i].fmax = g_th[i].ffast = 0;
}

void ps2_sched_block(void) {
    if (g_cur < 0) return;
    switch_out(TS_BLOCKED);
}

void ps2_sched_block_ef(uint32_t ef_uid) {
    if (g_cur < 0) return;
    g_th[g_cur].wait_ef = ef_uid;
    switch_out(TS_BLOCKED);
}

int ps2_sched_wake_ef(uint32_t ef_uid) {
    int n = 0;
    for (int i = 0; i < PS2_THREAD_MAX; i++)
        if (g_th[i].used && g_th[i].state == TS_BLOCKED && g_th[i].wait_ef == ef_uid) {
            g_th[i].state = TS_READY;
            g_th[i].wait_ef = 0;
            n++;
        }
    return n;
}

void ps2_sched_exit(uint32_t status) {
    if (g_cur < 0) return;
    g_th[g_cur].exit_status = status;
    g_th[g_cur].state = TS_DEAD;
    longjmp(g_sched_ctx, 1);
}

/* sceKernelDeleteThread: frees the slot of a thread that is not running. */
int ps2_sched_delete(uint32_t uid) {
    const int i = find(uid);
    if (i < 0 || i == g_cur) return -1;
    g_th[i].state = TS_DEAD;
    return 0;
}

int ps2_sched_in_thread(void) { return g_cur >= 0; }

int ps2_sched_wait_dead(uint32_t uid) {
    if (g_cur < 0) return 0;              /* module_start waits: pretend done */
    const int i = find(uid);
    if (i < 0) return -1;
    while (g_th[i].state != TS_DEAD) ps2_sched_yield();
    return 0;
}

void ps2_sched_dump_states(void) {
    psp_printf("[state]");
    for (int i = 0; i < PS2_THREAD_MAX; i++)
        if (g_th[i].used)
            psp_printf(" uid=0x%X st=%d ef=0x%X", g_th[i].uid, g_th[i].state, g_th[i].wait_ef);
    psp_printf("\n");
}

void ps2_sched_dump_stacks(void) {
    for (int i = 0; i < PS2_THREAD_MAX; i++)
        if (g_th[i].used)
            psp_printf("[stack] uid=0x%X max_used=%u of %u bytes\n",
                       g_th[i].uid, g_th[i].stack_used_max, g_th[i].estack_size);
}

int ps2_sched_pump(void) {
    int switches = 0;
    for (;;) {
        const int nxt = first_ready_except(g_cur);   /* rotate after current */
        if (nxt < 0) break;

        if (setjmp(g_sched_ctx) == 0) {
            ps2_thread *t = &g_th[nxt];
            g_cur = nxt;
            t->state = TS_RUNNING;
            t->switches++;
            switches++;

            if (!t->started) {
                t->started = 1;
                psp_cpu = t->cpu;
                g_slice_t0 = cop0_count();
                psp_printf("sched: first run uid=0x%08X sp=0x%08X top=0x%08X\n",
                           t->uid, t->cpu.r[PSP_REG_SP], t->top);
                ee_run_on_stack(thread_trampoline, t->top);
                /* not reached: the trampoline longjmps */
                t->state = TS_DEAD;
            } else {
                psp_cpu = t->cpu;
                g_slice_t0 = cop0_count();
                longjmp(t->ctx, 1);
            }
        } else slice_end();
        /* A thread yielded, exited or died; pick the next one. */
    }
    return switches;
}

int ps2_sched_ready(void) {
    int n = 0;
    for (int i = 0; i < PS2_THREAD_MAX; i++)
        if (g_th[i].used && g_th[i].state == TS_READY) n++;
    return n;
}

int ps2_sched_live(void) {
    int n = 0;
    for (int i = 0; i < PS2_THREAD_MAX; i++)
        if (g_th[i].used && g_th[i].state != TS_DEAD) n++;
    return n;
}

int ps2_sched_created(void) { return g_created; }

uint32_t ps2_sched_current_uid(void) {
    return (g_cur >= 0) ? g_th[g_cur].uid : 0;
}

const char *ps2_sched_current_name(void) {
    return (g_cur >= 0) ? g_th[g_cur].name : "main";
}
