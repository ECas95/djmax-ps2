/* DJMAX Fever (PS2 port) -- minimal psprecomp runtime implementation.
 *
 * See runtime_min.h for what this is and what it deliberately is not.
 */
#include "runtime_min.h"
#include "ps2_audio.h"
void ps2_sched_dump_states(void);

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/* ======================================================================
 * Console output: the EE TTY register, not ps2sdk printf.
 *
 * ps2sdk printf goes through IOP FILEIO and reaches nothing in a plain ELF
 * boot (measured). A byte written to 0x1000F180 does reach PCSX2's log. The
 * formatter below is self-contained on purpose: newlib's vsnprintf pulls in
 * stdio state and, with a 31 MB image, its malloc traffic was what tipped the
 * EE heap over during bring-up.
 * ==================================================================== */
#define EE_TTY (*(volatile unsigned char *)0x1000F180)

static void tty_putc(char c) { EE_TTY = (unsigned char)c; }

void psp_tty_write(const char *s) {
    while (*s) tty_putc(*s++);
}

static void tty_u64(uint64_t v, unsigned base, int upper, int width, int zero) {
    char buf[32];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;

    if (v == 0) buf[n++] = '0';
    while (v) { buf[n++] = digits[v % base]; v /= base; }
    while (width > n) { tty_putc(zero ? '0' : ' '); width--; }
    while (n) tty_putc(buf[--n]);
}

void psp_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    for (; *fmt; fmt++) {
        if (*fmt != '%') { tty_putc(*fmt); continue; }
        fmt++;

        int zero = 0, width = 0, len = 0;
        if (*fmt == '0') { zero = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        if (*fmt == 'l') { len = 1; fmt++; if (*fmt == 'l') { len = 2; fmt++; } }

        switch (*fmt) {
        case 'd': case 'i': {
            long long v = (len == 2) ? va_arg(ap, long long)
                        : (len == 1) ? (long long)va_arg(ap, long)
                                     : (long long)va_arg(ap, int);
            if (v < 0) { tty_putc('-'); v = -v; }
            tty_u64((uint64_t)v, 10, 0, width, zero);
            break;
        }
        case 'u': {
            unsigned long long v = (len == 2) ? va_arg(ap, unsigned long long)
                                 : (len == 1) ? (unsigned long long)va_arg(ap, unsigned long)
                                              : (unsigned long long)va_arg(ap, unsigned);
            tty_u64(v, 10, 0, width, zero);
            break;
        }
        case 'x': case 'X': {
            unsigned long long v = (len == 2) ? va_arg(ap, unsigned long long)
                                 : (len == 1) ? (unsigned long long)va_arg(ap, unsigned long)
                                              : (unsigned long long)va_arg(ap, unsigned);
            tty_u64(v, 16, *fmt == 'X', width, zero);
            break;
        }
        case 'p': tty_u64((uint64_t)(uintptr_t)va_arg(ap, void *), 16, 0, 8, 1); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s) tty_putc(*s++);
            break;
        }
        case 'c': tty_putc((char)va_arg(ap, int)); break;
        case '%': tty_putc('%'); break;
        case 0: va_end(ap); return;
        default: tty_putc('%'); tty_putc(*fmt); break;
        }
    }
    va_end(ap);
}

/* ======================================================================
 * CPU state
 * ==================================================================== */
psp_cpu_state psp_cpu;

void psp_cpu_reset(void) {
    memset(&psp_cpu, 0, sizeof psp_cpu);
}

/* ======================================================================
 * Memory
 * ==================================================================== */
static uint8_t g_ram    [PSP_RAM_SIZE]    __attribute__((aligned(64)));
static uint8_t g_module [PSP_MODULE_SIZE] __attribute__((aligned(64)));
static uint8_t g_scratch[PSP_SCRATCH_SIZE]__attribute__((aligned(64)));

psp_memory psp_mem;
uint64_t   psp_mem_bad_access;

int psp_mem_init(void) {
    psp_mem.ram     = g_ram;
    psp_mem.module  = g_module;
    psp_mem.scratch = g_scratch;
    memset(g_ram, 0, sizeof g_ram);
    memset(g_module, 0, sizeof g_module);
    memset(g_scratch, 0, sizeof g_scratch);
    psp_mem_bad_access = 0;
    return 0;
}

void psp_mem_free(void) { /* static backing store */ }

/* The guest reads its module image (code, .rodata, .data, .cplinit) at the
 * addresses the EBOOT linked at, i.e. from 0. This build embeds the two
 * PT_LOAD segments of the EBOOT and copies them here at boot. */
int psp_mem_map_module(uint32_t base, uint32_t size) {
    (void)base; (void)size;
    return 0;
}

int psp_mem_load_module(const void *seg0, uint32_t len0,
                        const void *seg1, uint32_t len1,
                        uint32_t seg1_vaddr, uint32_t bss_len) {
    if (!seg0 || !seg1) return -1;
    if (len0 > PSP_MODULE_SIZE || seg1_vaddr + len1 + bss_len > PSP_MODULE_SIZE)
        return -1;
    memcpy(psp_mem.module, seg0, len0);
    memcpy(psp_mem.module + seg1_vaddr, seg1, len1);
    memset(psp_mem.module + seg1_vaddr + len1, 0, bss_len);
    return 0;
}

const char *psp_str(uint32_t addr, char *dst, size_t cap) {
    size_t i = 0;
    if (!dst || cap == 0) return dst;
    for (; i + 1 < cap; i++) {
        char c = (char)psp_read8(addr + (uint32_t)i);
        dst[i] = c;
        if (c == 0) return dst;
    }
    dst[i] = 0;
    return dst;
}

void *psp_mem_ptr(uint32_t addr, uint32_t size) {
    const uint32_t a = addr & PSP_ADDR_MASK;

    if (a >= PSP_SCRATCH_BASE && (a - PSP_SCRATCH_BASE) + size <= PSP_SCRATCH_SIZE)
        return psp_mem.scratch + (a - PSP_SCRATCH_BASE);

    if (a < PSP_MODULE_SIZE && a + size <= PSP_MODULE_SIZE)
        return psp_mem.module + a;

    if (a >= PSP_RAM_BASE && (a - PSP_RAM_BASE) + size <= PSP_RAM_SIZE)
        return psp_mem.ram + (a - PSP_RAM_BASE);

    psp_mem_bad_access++;
    { static uint32_t n; if (n < 16) { n++;
        psp_printf("[bad] addr=0x%08X size=%u pc=0x%08X ra=0x%08X\n", addr, size, psp_cpu.pc, psp_cpu.r[PSP_REG_RA]); } }
    return NULL;
}

/* Hot path: every guest load/store lands here. Main RAM first (stack, heap), then the
 * module image past the scratchpad window (globals), then the general lookup. Same
 * mapping as psp_mem_ptr, which keeps the scratchpad-before-module precedence. */
typedef struct { uint16_t v; } __attribute__((packed, may_alias)) u16_un;
typedef struct { uint32_t v; } __attribute__((packed, may_alias)) u32_un;
#define MOD_FAST_LO (PSP_SCRATCH_BASE + PSP_SCRATCH_SIZE)
static inline __attribute__((always_inline)) uint8_t *fast_ptr(uint32_t addr, uint32_t size) {
    const uint32_t a = addr & PSP_ADDR_MASK;
    const uint32_t o = a - PSP_RAM_BASE;
    if (o <= PSP_RAM_SIZE - size) return g_ram + o;
    if (a - MOD_FAST_LO <= PSP_MODULE_SIZE - MOD_FAST_LO - size) return g_module + a;
    return (uint8_t *)psp_mem_ptr(addr, size);
}

uint8_t psp_read8(uint32_t addr) {
    const uint8_t *p = fast_ptr(addr, 1);
    return p ? *p : 0;
}
uint16_t psp_read16(uint32_t addr) {
    const uint8_t *p = fast_ptr(addr, 2);
    return p ? ((const u16_un *)p)->v : 0;
}
uint32_t psp_read32(uint32_t addr) {
    const uint8_t *p = fast_ptr(addr, 4);
    return p ? ((const u32_un *)p)->v : 0;
}
float psp_read_f32(uint32_t addr) {
    uint32_t b = psp_read32(addr);
    return psp_bits_to_f32(b);
}

/* TEMP write watch: [g_watch_lo, g_watch_lo + g_watch_len) logged with ra (main.c sets it). */
uint32_t g_watch_lo, g_watch_len, g_watch_n;
static void watch_hit(uint32_t addr, uint32_t val, uint32_t size) {
    if (g_watch_n >= 200u) return;
    g_watch_n++;
    psp_printf("[watch] w%u addr=0x%08X val=0x%08X ra=0x%08X sp=0x%08X\n", size, addr, val,
               psp_cpu.r[PSP_REG_RA], psp_cpu.r[PSP_REG_SP]);
    if (psp_cpu.r[PSP_REG_RA] == 0x000047C4u && g_watch_n <= 3u) {   /* VCE layer draw: its state */
        const uint32_t l = psp_cpu.r[17];
        char line[200]; int k = 0;
        for (uint32_t o = 0; o < 116u; o += 4u)
            k += snprintf(line + k, sizeof line - (size_t)k, " %d", (int)psp_bits_to_f32(psp_read32(l + o)));
        psp_printf("[watch]   layer 0x%08X:%s\n", l, line);
    }
}
#define WATCH(a, v, n) do { if (((a) & PSP_ADDR_MASK) - g_watch_lo < g_watch_len) watch_hit((a), (v), (n)); } while (0)

void psp_write8(uint32_t addr, uint8_t val) {
    uint8_t *p = fast_ptr(addr, 1);
    if (p) *p = val;
}
void psp_write16(uint32_t addr, uint16_t val) {
    uint8_t *p = fast_ptr(addr, 2);
    if (p) ((u16_un *)p)->v = val;
}
void psp_write32(uint32_t addr, uint32_t val) {
    uint8_t *p = fast_ptr(addr, 4);
    if (p) ((u32_un *)p)->v = val;
}
void psp_write_f32(uint32_t addr, float val) {
    psp_write32(addr, psp_f32_to_bits(val));
}

int psp_mem_write_block(uint32_t addr, const void *src, uint32_t len) {
    void *p = psp_mem_ptr(addr, len);
    if (!p) return -1;
    memcpy(p, src, len);
    return 0;
}
int psp_mem_read_block(void *dst, uint32_t addr, uint32_t len) {
    const void *p = psp_mem_ptr(addr, len);
    if (!p) return -1;
    memcpy(dst, p, len);
    return 0;
}

/* ======================================================================
 * Dispatch table (same open-addressing scheme as psprecomp)
 * ==================================================================== */
#define DISPATCH_CAP 65536u   /* power of two; 31 224 entries registered today */

/* 8 bytes per slot instead of 12: with 65 536 slots that is 0.5 MB instead of
 * 0.79 MB of .bss, and .bss is the scarce resource next to the guest RAM. */
static uint32_t g_addr[DISPATCH_CAP];
static psp_fn_t g_fn[DISPATCH_CAP];      /* NULL == empty slot */
static uint32_t g_count;
static uint64_t g_misses;
static uint32_t g_last;
static psp_miss_fn_t g_miss;
static uint32_t g_low_calls;              /* indirect calls to 0x000..0xFFF */
static uint32_t g_low_addrs[8];
/* .cplinit entries read from the module image (seg1 at 0x000E15D8). */
static const uint32_t g_ctor[7] = { 0xDF4u, 0x2C440u, 0x6A744u, 0x740D0u,
                                    0x76F48u, 0x8889Cu, 0x8E1D4u };
static uint32_t g_ctor_hits;

static uint32_t hash_addr(uint32_t a) {
    return (a >> 2) * 2654435761u;
}

void psp_register(uint32_t addr, psp_fn_t fn) {
    uint32_t m = DISPATCH_CAP - 1;
    uint32_t j = hash_addr(addr) & m;
    while (g_fn[j]) {
        if (g_addr[j] == addr) { g_fn[j] = fn; return; }
        j = (j + 1) & m;
    }
    g_addr[j] = addr;
    g_fn[j] = fn;
    g_count++;
}

void psp_register_label(uint32_t addr, psp_fn_t fn) {
    uint32_t m = DISPATCH_CAP - 1;
    uint32_t j = hash_addr(addr) & m;
    while (g_fn[j]) {
        if (g_addr[j] == addr) return;
        j = (j + 1) & m;
    }
    g_addr[j] = addr;
    g_fn[j] = fn;
    g_count++;
}

psp_fn_t psp_lookup(uint32_t addr) {
    uint32_t m = DISPATCH_CAP - 1;
    uint32_t j = hash_addr(addr) & m;
    while (g_fn[j]) {
        if (g_addr[j] == addr) return g_fn[j];
        j = (j + 1) & m;
    }
    return NULL;
}

/* Budget: a game's main loop never returns, and on the EE there is no host to
 * kill the process. main() arms this and longjmps out of it. */
static uint64_t g_calls, g_budget;
static void (*g_over)(void);

void psp_dispatch_set_budget(uint64_t calls, void (*on_exceeded)(void)) {
    g_budget = calls;
    g_over = on_exceeded;
    g_calls = 0;
}
uint64_t psp_dispatch_calls(void) { return g_calls; }

void psp_set_miss_handler(psp_miss_fn_t fn) { g_miss = fn; }

void psp_dispatch(uint32_t addr) {
    g_last = addr;
    if (addr < 0x1000u) {
        if (g_low_calls < 8) g_low_addrs[g_low_calls] = addr;
        g_low_calls++;
    }
    for (int i = 0; i < 7; i++)
        if (addr == g_ctor[i]) { g_ctor_hits++; break; }
    psp_cpu.pc = addr;

    if (g_budget && ++g_calls >= g_budget) {
        g_budget = 0;
        if (g_over) g_over();
    }

    psp_fn_t fn = psp_lookup(addr);
    if (fn) { fn(); return; }

    g_misses++;
    if (g_miss) g_miss(addr);
}

void psp_call_from_host(uint32_t addr) {
    psp_frame f = { 0, psp_cpu.frame };
    psp_cpu.frame = &f;
    psp_dispatch(addr);
    psp_cpu.frame = f.prev;
    psp_cpu.unwind = 0;
}

int psp_ra_is_jump(uint32_t ra) {
    const psp_frame *f = psp_cpu.frame;
    if (!f || !f->ret || ra == f->ret) return 0;
    for (const psp_frame *g = f->prev; g && g->ret; g = g->prev)
        if (g->ret == ra) { psp_cpu.unwind = ra; return 0; }
    return 1;
}

uint32_t psp_dispatch_count(void)  { return g_count; }
uint64_t psp_dispatch_misses(void) { return g_misses; }
uint64_t psp_dispatch_last(void)   { return g_last; }
uint32_t psp_dispatch_low_calls(void) { return g_low_calls; }
uint32_t psp_dispatch_ctor_hits(void) { return g_ctor_hits; }
uint32_t psp_dispatch_low_addr(int i) { return (i >= 0 && i < 8) ? g_low_addrs[i] : 0; }

void psp_dispatch_reset(void) {
    memset(g_fn, 0, sizeof g_fn);
    g_count = 0;
    g_misses = 0;
    g_last = 0;
    g_miss = NULL;
    g_budget = 0;
    g_calls = 0;
}

/* Stub accounting, filled by build.sh's generated stubs. */
static uint64_t g_stub_calls;
void psp_stub_hit(uint32_t addr) { (void)addr; g_stub_calls++; }
uint64_t psp_stub_calls(void) { return g_stub_calls; }

/* ======================================================================
 * Traps
 * ==================================================================== */
#define TRAP_LOGGED 8
static uint64_t g_syscall_total, g_unimplemented_total;
static uint32_t g_syscall_logged, g_unimplemented_logged;

void psp_syscall(uint32_t id) {
    if (g_syscall_logged < TRAP_LOGGED) {
        g_syscall_logged++;
        printf("ps2: syscall 0x%05X\n", id);
    }
    g_syscall_total++;
}

void psp_unimplemented(uint32_t addr, const char *what) {
    if (g_unimplemented_logged < TRAP_LOGGED) {
        g_unimplemented_logged++;
        printf("ps2: unimplemented at 0x%08X: %s\n", addr, what ? what : "?");
    }
    g_unimplemented_total++;
}

uint64_t psp_syscall_total(void)       { return g_syscall_total; }
uint64_t psp_unimplemented_total(void) { return g_unimplemented_total; }

void psp_trap_reset(void) {
    g_syscall_total = g_unimplemented_total = 0;
    g_syscall_logged = g_unimplemented_logged = 0;
}

/* ======================================================================
 * HLE: per-NID counters, and a handler when one is registered.
 * psp_hle_call is the only path an import thunk takes.
 * ==================================================================== */
#define HLE_MAX 512
typedef struct {
    uint32_t nid;
    uint32_t calls;
    uint64_t returns_zero;
    const char *name;
    psp_hle_fn fn;
} hle_slot;

static hle_slot g_hle[HLE_MAX];
static int      g_hle_n;
static uint64_t g_hle_zero_calls;
static uint64_t g_hle_unimpl;

static int hle_find(uint32_t nid) {
    for (int i = 0; i < g_hle_n; i++)
        if (g_hle[i].nid == nid) return i;
    return -1;
}

static int hle_slot_for(uint32_t nid) {
    int i = hle_find(nid);
    if (i >= 0) return i;
    if (g_hle_n >= HLE_MAX) return -1;
    i = g_hle_n++;
    g_hle[i].nid = nid;
    g_hle[i].calls = 0;
    g_hle[i].returns_zero = 0;
    g_hle[i].name = NULL;
    g_hle[i].fn = NULL;
    return i;
}

void psp_hle_register(uint32_t nid, const char *lib, const char *name, psp_hle_fn fn) {
    (void)lib;
    int i = hle_slot_for(nid);
    if (i >= 0) { g_hle[i].name = name; g_hle[i].fn = fn; }
}
void psp_hle_register_unnamed(uint32_t nid, const char *lib, psp_hle_fn fn) {
    (void)lib;
    int i = hle_slot_for(nid);
    if (i >= 0) g_hle[i].fn = fn;
}

/* Implemented by ps2_hle.c; registers the real firmware handlers. */
extern void ps2_hle_install(void);

/* Overridden by the generated ps2_imports.c in game builds; kept weak so the
 * older `full`/`subset` modes (no import table) still link. */
__attribute__((weak)) void ps2_imports_register(void) { }

void psp_hle_init(void) { ps2_hle_install(); }

const char *psp_hle_name(uint32_t nid) {
    int i = hle_find(nid);
    return i >= 0 ? g_hle[i].name : NULL;
}

int psp_hle_count(void) { return g_hle_n; }

/* Circular trace of the last 32 imports: NID, args, caller RA and the value
 * handed back to the guest. Dumped when the guest enters its exit path. */
typedef struct {
    uint32_t nid, a0, a1, a2, a3, ra, ret;
} hle_rec;
static hle_rec  g_ring[32];
static uint32_t g_ring_n;

uint32_t psp_hle_last_nid(int back) {
    if (back < 0 || back >= 32 || back >= (int)g_ring_n) return 0;
    return g_ring[(g_ring_n - 1u - (uint32_t)back) & 31u].nid;
}

void psp_hle_dump_ring(void) {
    const uint32_t n = g_ring_n < 32u ? g_ring_n : 32u;
    const uint32_t first = g_ring_n - n;
    psp_printf("[hle-ring] %u of %u calls, oldest first:\n", n, g_ring_n);
    for (uint32_t k = 0; k < n; k++) {
        const hle_rec *r = &g_ring[(first + k) & 31u];
        psp_printf("  %08X a0=%08X a1=%08X a2=%08X a3=%08X ret=%08X ra=%08X\n",
                   r->nid, r->a0, r->a1, r->a2, r->a3, r->ret, r->ra);
    }
}

/* Called from the guest at every import thunk; the RA names the caller. */
static uint32_t g_last_hle_ra;
uint32_t psp_hle_last_ra(void) { return g_last_hle_ra; }

/* Without per-instruction probes the only cheap run bound is the HLE call
 * count; main arms psp_stop_now() to longjmp out and print the report. */
#define HLE_CALL_LIMIT 3000000ull
extern void psp_stop_now(void) __attribute__((weak));

void psp_hle_call(uint32_t nid) {
    /* A PSP audio thread preempts the game when its wait is over. Here the switch can
     * only happen at a firmware call: during loading between screens user_main makes
     * many (file reads) and the sound threads would otherwise wait for the whole load. */
    ps2_audio_preempt();
    hle_rec *rec = &g_ring[g_ring_n++ & 31u];
    rec->nid = nid;
    rec->a0 = psp_cpu.r[PSP_REG_A0];
    rec->a1 = psp_cpu.r[PSP_REG_A1];
    rec->a2 = psp_cpu.r[PSP_REG_A2];
    rec->a3 = psp_cpu.r[PSP_REG_A3];
    rec->ra = psp_cpu.r[PSP_REG_RA];
    rec->ret = 0;
    g_last_hle_ra = rec->ra;
    int i = hle_slot_for(nid);
    if (i >= 0) g_hle[i].calls++;
    g_hle_zero_calls++;
    if ((g_hle_zero_calls % 500000ull) == 0 && g_hle_zero_calls) {
        psp_hle_dump_counts(5);
        ps2_sched_dump_states();
    }
    if ((g_hle_zero_calls % 100000ull) == 0)
        psp_printf("[hb] calls=%llu pc=0x%08X last_ra=0x%08X\n",
                   (unsigned long long)g_hle_zero_calls, psp_cpu.pc, psp_cpu.r[PSP_REG_RA]);
    if (i >= 0 && g_hle[i].fn) {
        g_hle[i].fn();
        rec->ret = psp_cpu.r[PSP_REG_V0];
        if (g_ring_n <= 90)
            psp_printf("[call %u] NID %08X -> v0=%08X ra=%08X\n",
                       (unsigned)g_ring_n, nid, psp_cpu.r[PSP_REG_V0], g_last_hle_ra);
        return;
    }
    if (!psp_hle_name(nid)) g_hle_unimpl++;
    { static uint32_t seen[96]; static uint32_t nseen; uint32_t k = 0;
      for (; k < nseen && seen[k] != nid; k++) { }
      if (k == nseen && nseen < 96) {
          seen[nseen++] = nid;
          psp_printf("[nohle] NID %08X a0=%08X a1=%08X ra=%08X\n", nid, rec->a0, rec->a1, rec->ra);
      } }
    /* Unimplemented: report success and 0, like psprecomp. */
    psp_cpu.r[PSP_REG_V0] = 0;
    rec->ret = 0;
}

uint64_t psp_hle_unimplemented_calls(void) { return g_hle_unimpl; }
uint64_t psp_hle_zero_calls(void)          { return g_hle_zero_calls; }

uint64_t psp_hle_total_calls(void) { return g_hle_zero_calls; }

uint32_t psp_hle_calls(uint32_t nid) {
    int i = hle_find(nid);
    return (i >= 0) ? g_hle[i].calls : 0;
}

int psp_hle_distinct(void) {
    int n = 0;
    for (int i = 0; i < g_hle_n; i++) if (g_hle[i].calls) n++;
    return n;
}

const char *psp_hle_name_of(uint32_t nid) {
    int i = hle_find(nid);
    return (i >= 0) ? g_hle[i].name : NULL;
}

void psp_hle_unimplemented_reset(void) { g_hle_unimpl = 0; }

void psp_hle_dump_counts(int limit) {
    /* Top `limit` by call count. 512 rows: selection sort is fine. */
    uint8_t used[HLE_MAX];
    memset(used, 0, sizeof used);
    for (int row = 0; row < limit && row < g_hle_n; row++) {
        int best = -1;
        for (int i = 0; i < g_hle_n; i++) {
            if (used[i] || g_hle[i].calls == 0) continue;
            if (best < 0 || g_hle[i].calls > g_hle[best].calls) best = i;
        }
        if (best < 0) break;
        used[best] = 1;
        psp_printf("  NID 0x%08X  %10u calls  %s\n",
                g_hle[best].nid, g_hle[best].calls,
                g_hle[best].name ? g_hle[best].name : "(unimplemented)");
    }
}

/* ======================================================================
 * VFPU -- only the entry points this corpus references, no prefixes.
 * ==================================================================== */
static void lanes4(uint32_t vreg, int size, int out[4]) {
    const int mtx = (vreg >> 2) & 7;
    const int col = vreg & 3;
    int transpose = (vreg >> 5) & 1, row;

    switch (size) {
    case 1:  row = (vreg >> 5) & 3; transpose = 0; break;
    case 3:  row = (vreg >> 6) & 1;                break;
    default: row = (vreg >> 5) & 2;                break;
    }
    for (int i = 0; i < 4; i++) {
        const int step = (row + i) & 3;
        out[i] = transpose ? mtx * 4 + step * 32 + col
                           : mtx * 4 + col  * 32 + step;
    }
}

int psp_vfpu_regs(uint32_t vreg, int size, int out[4]) {
    int l[4];
    const int len = (size >= 1 && size <= 4) ? size : 4;
    lanes4(vreg, len, l);
    for (int i = 0; i < len; i++) out[i] = l[i];
    return len;
}

static void vread(uint32_t vreg, int size, float out[4]) {
    int l[4];
    psp_vfpu_regs(vreg, size, l);
    for (int i = 0; i < size; i++) out[i] = psp_cpu.v[l[i]];
}
static void vwrite(uint32_t vreg, int size, const float in[4]) {
    int l[4];
    psp_vfpu_regs(vreg, size, l);
    for (int i = 0; i < size; i++) psp_cpu.v[l[i]] = in[i];
}

static void matrix_cols(uint32_t vd, int size, int cols[4][4]) {
    const uint32_t mtx = (vd >> 2) & 7;
    const uint32_t transpose = (vd >> 5) & 1;
    for (int c = 0; c < size; c++) {
        uint32_t vreg = (mtx << 2) | (uint32_t)c | (transpose << 5);
        psp_vfpu_regs(vreg, size, cols[c]);
    }
}
static void matrix_read(uint32_t v, int size, float m[4][4]) {
    int cols[4][4];
    matrix_cols(v, size, cols);
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) m[c][r] = psp_cpu.v[cols[c][r]];
}
static void matrix_write(uint32_t v, int size, const float m[4][4]) {
    int cols[4][4];
    matrix_cols(v, size, cols);
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) psp_cpu.v[cols[c][r]] = m[c][r];
}

void psp_lv_q(uint32_t vt, uint32_t addr) {
    int r[4];
    psp_vfpu_regs(vt, 4, r);
    addr &= ~15u;
    for (int i = 0; i < 4; i++) psp_cpu.v[r[i]] = psp_read_f32(addr + (uint32_t)i * 4);
}
void psp_sv_s(uint32_t vt, uint32_t addr) {
    int r[4];
    psp_vfpu_regs(vt, 1, r);
    psp_write_f32(addr & ~3u, psp_cpu.v[r[0]]);
}

void psp_vmul(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    float s[4], t[4], o[4];
    vread(vs, size, s); vread(vt, size, t);
    for (int i = 0; i < size; i++) o[i] = s[i] * t[i];
    vwrite(vd, size, o);
}
void psp_vmin(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    float s[4], t[4], o[4];
    vread(vs, size, s); vread(vt, size, t);
    for (int i = 0; i < size; i++) o[i] = s[i] < t[i] ? s[i] : t[i];
    vwrite(vd, size, o);
}
void psp_vsge(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    float s[4], t[4], o[4];
    vread(vs, size, s); vread(vt, size, t);
    for (int i = 0; i < size; i++) o[i] = (s[i] >= t[i]) ? 1.0f : 0.0f;
    vwrite(vd, size, o);
}
void psp_vscl(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    float s[4], t[4], o[4];
    vread(vs, size, s); vread(vt, 1, t);
    for (int i = 0; i < size; i++) o[i] = s[i] * t[0];
    vwrite(vd, size, o);
}
void psp_vhdp(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    float s[4], t[4], o[4] = { 0, 0, 0, 0 };
    vread(vs, size, s); vread(vt, size, t);
    for (int i = 0; i < size - 1; i++) o[0] += s[i] * t[i];
    o[0] += t[size - 1];
    vwrite(vd, 1, o);
}
void psp_vtfm(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    float m[4][4], in[4], out[4];
    int d[4], t[4];
    matrix_read(vs, size, m);
    psp_vfpu_regs(vt, size, t);
    psp_vfpu_regs(vd, size, d);
    for (int i = 0; i < size; i++) in[i] = psp_cpu.v[t[i]];
    for (int r = 0; r < size; r++) {
        float sum = 0.0f;
        for (int c = 0; c < size; c++) sum += m[c][r] * in[c];
        out[r] = sum;
    }
    for (int r = 0; r < size; r++) psp_cpu.v[d[r]] = out[r];
}
void psp_vmmul(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    float a[4][4], b[4][4], out[4][4];
    matrix_read(vs, size, a);
    matrix_read(vt, size, b);
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) {
            float sum = 0.0f;
            for (int k = 0; k < size; k++) sum += a[r][k] * b[c][k];
            out[c][r] = sum;
        }
    matrix_write(vd, size, out);
}

void psp_vcmp(uint32_t cond, uint32_t vs, uint32_t vt, int size) {
    float s[4], t[4];
    uint32_t cc = 0;
    int all = 1, any = 0;
    vread(vs, size, s); vread(vt, size, t);
    for (int i = 0; i < size; i++) {
        const float a = s[i], b = t[i];
        const int nan = a != a, inf = (a == INFINITY) || (a == -INFINITY);
        int r;
        switch (cond & 0xF) {
        case 0:  r = 0;           break;   /* FL  */
        case 1:  r = (a == b);    break;   /* EQ  */
        case 2:  r = (a <  b);    break;   /* LT  */
        case 3:  r = (a <= b);    break;   /* LE  */
        case 4:  r = 1;           break;   /* TR  */
        case 5:  r = (a != b);    break;   /* NE  */
        case 6:  r = (a >= b);    break;   /* GE  */
        case 7:  r = (a >  b);    break;   /* GT  */
        case 8:  r = (a == 0.0f); break;   /* EZ  */
        case 9:  r = nan;         break;   /* EN  */
        case 10: r = inf;         break;   /* EI  */
        case 11: r = nan || inf;  break;   /* ES  */
        case 12: r = !(a == 0.0f);break;   /* NZ  */
        case 13: r = !nan;        break;   /* NN  */
        case 14: r = !inf;        break;   /* NI  */
        default: r = !(nan || inf); break; /* NS  */
        }
        if (r) { cc |= 1u << i; any = 1; }
        else all = 0;
    }
    if (any) cc |= 1u << 4;   /* vcmp "any" */
    if (all) cc |= 1u << 5;   /* vcmp "all" */
    psp_cpu.vfpu_cc = cc;
}
