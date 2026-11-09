/* DJMAX Fever (PS2 port) -- minimal replacement for the psprecomp runtime.
 *
 * The recompiled corpus was emitted against <psprecomp/{cpu,mem,recomp_rt,
 * dispatch,vfpu,hle}.h>. This header supplies the same names, but only the
 * parts the corpus actually needs, so that the 117 generated translation
 * units compile on the EE with ee-gcc and link against ps2sdk.
 *
 * Differences from the host runtime, and why:
 *   - The EE has 32 MB of RDRAM total, and the recompiled code lives in it.
 *     A 1:1 model of the PSP's 32 MB RAM plus 2 MB VRAM is impossible. The
 *     guest PSP address space is mapped onto a 4 MB module window (addresses
 *     below 0x00400000, where the emitted literals point) and an 8 MB RAM
 *     window at 0x08000000. Anything else reads 0 and counts as a bad access.
 *   - No HLE: every import is a counter plus "return 0", which is exactly the
 *     PSPRECOMP behaviour for an unimplemented NID. The point of this build is
 *     to reach the entry point, not to run the game.
 *   - VFPU: only the ten entry points the corpus references are implemented,
 *     without prefix support (the corpus contains no vpfx* instructions).
 *
 * Everything else (division edge cases, unaligned load/store, dispatch
 * semantics) is copied from psprecomp's headers/sources so the behaviour is
 * the one the corpus was written and tested against.
 */
#ifndef DJMX_PS2_RUNTIME_MIN_H
#define DJMX_PS2_RUNTIME_MIN_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================
 * cpu.h
 * ==================================================================== */
enum {
    PSP_REG_ZERO = 0, PSP_REG_AT,
    PSP_REG_V0, PSP_REG_V1,
    PSP_REG_A0, PSP_REG_A1, PSP_REG_A2, PSP_REG_A3,
    PSP_REG_T0, PSP_REG_T1, PSP_REG_T2, PSP_REG_T3,
    PSP_REG_T4, PSP_REG_T5, PSP_REG_T6, PSP_REG_T7,
    PSP_REG_S0, PSP_REG_S1, PSP_REG_S2, PSP_REG_S3,
    PSP_REG_S4, PSP_REG_S5, PSP_REG_S6, PSP_REG_S7,
    PSP_REG_T8, PSP_REG_T9,
    PSP_REG_K0, PSP_REG_K1,
    PSP_REG_GP, PSP_REG_SP, PSP_REG_FP, PSP_REG_RA,
    PSP_NUM_GPR = 32
};

typedef struct psp_frame {
    uint32_t ret;
    struct psp_frame *prev;
} psp_frame;

typedef struct {
    uint32_t r[PSP_NUM_GPR];
    uint32_t hi, lo;
    uint32_t pc;               /* only meaningful at dispatch boundaries */

    float    f[32];            /* COP1 */
    uint32_t fcr31;

    float    v[128];           /* VFPU: 8 matrices x 4 rows x 4 columns */
    uint32_t vfpu_cc;

    psp_frame *frame;
    uint32_t   unwind;
} psp_cpu_state;

extern psp_cpu_state psp_cpu;

static inline void psp_set_reg(uint32_t idx, uint32_t val) {
    if (idx != PSP_REG_ZERO) psp_cpu.r[idx] = val;
}

void psp_cpu_reset(void);

#define PSP_FCR31_C (1u << 23)
static inline int  psp_fpu_cond(void)      { return (psp_cpu.fcr31 & PSP_FCR31_C) != 0; }
static inline void psp_fpu_set_cond(int c) {
    psp_cpu.fcr31 = c ? (psp_cpu.fcr31 | PSP_FCR31_C) : (psp_cpu.fcr31 & ~PSP_FCR31_C);
}

/* ======================================================================
 * mem.h
 * ==================================================================== */

#define PSP_ADDR_MASK       0x3FFFFFFFu

/* Backing stores, not the console's full map: see the file comment. */
#define PSP_MODULE_BASE     0x00000000u
#define PSP_MODULE_SIZE     0x00540000u  /* 5.5 MB: EBOOT image ends at 0x0053E478 */
#define PSP_RAM_BASE        0x08000000u
#define PSP_RAM_SIZE        (19u * 1024u * 1024u)  /* 19 MB: VPL 15.75 + partition + buffers */
#define PSP_SCRATCH_BASE    0x00010000u
#define PSP_SCRATCH_SIZE    0x00004000u

typedef struct {
    uint8_t *ram;
    uint8_t *module;
    uint8_t *scratch;
} psp_memory;

extern psp_memory psp_mem;


int  psp_mem_init(void);
void psp_mem_free(void);
int  psp_mem_map_module(uint32_t base, uint32_t size);
int  psp_mem_load_module(const void *seg0, uint32_t len0,
                          const void *seg1, uint32_t len1,
                          uint32_t seg1_vaddr, uint32_t bss_len);

void *psp_mem_ptr(uint32_t addr, uint32_t size);

extern uint64_t psp_mem_bad_access;

uint8_t  psp_read8 (uint32_t addr);
uint16_t psp_read16(uint32_t addr);
uint32_t psp_read32(uint32_t addr);
float    psp_read_f32(uint32_t addr);

void psp_write8 (uint32_t addr, uint8_t  val);
void psp_write16(uint32_t addr, uint16_t val);
void psp_write32(uint32_t addr, uint32_t val);
void psp_write_f32(uint32_t addr, float val);

int psp_mem_write_block(uint32_t addr, const void *src, uint32_t len);
int psp_mem_read_block(void *dst, uint32_t addr, uint32_t len);

/* o32 argument access, exactly as psprecomp's hle.h: arguments 0-3 arrive in
 * $a0-$a3, 4-7 in $t0-$t3 (PSP firmware stubs do this, plain o32 does not). */
static inline uint32_t psp_arg(int n) {
    if (n < 4) return psp_cpu.r[PSP_REG_A0 + n];
    if (n < 8) return psp_cpu.r[PSP_REG_T0 + (n - 4)];
    return psp_read32(psp_cpu.r[PSP_REG_SP] + (uint32_t)n * 4);
}
static inline void psp_ret(uint32_t v) { psp_cpu.r[PSP_REG_V0] = v; }

/* Read a NUL-terminated string out of guest memory; always terminates. */
const char *psp_str(uint32_t addr, char *dst, size_t cap);

/* ======================================================================
 * recomp_rt.h  (semantic helpers -- verbatim from psprecomp)
 * ==================================================================== */

static inline uint32_t psp_add(uint32_t a, uint32_t b) { return a + b; }
static inline uint32_t psp_sub(uint32_t a, uint32_t b) { return a - b; }

static inline uint32_t psp_slt (uint32_t a, uint32_t b) { return (int32_t)a < (int32_t)b; }
static inline uint32_t psp_sltu(uint32_t a, uint32_t b) { return a < b; }
static inline uint32_t psp_max (uint32_t a, uint32_t b) { return (int32_t)a > (int32_t)b ? a : b; }
static inline uint32_t psp_min (uint32_t a, uint32_t b) { return (int32_t)a < (int32_t)b ? a : b; }

static inline uint32_t psp_sll(uint32_t v, uint32_t s) { return v << (s & 31); }
static inline uint32_t psp_srl(uint32_t v, uint32_t s) { return v >> (s & 31); }
static inline uint32_t psp_sra(uint32_t v, uint32_t s) { return (uint32_t)((int32_t)v >> (s & 31)); }
static inline uint32_t psp_rotr(uint32_t v, uint32_t s) {
    s &= 31;
    return s ? ((v >> s) | (v << (32 - s))) : v;
}

static inline void psp_mult(uint32_t a, uint32_t b) {
    int64_t r = (int64_t)(int32_t)a * (int64_t)(int32_t)b;
    psp_cpu.lo = (uint32_t)r;
    psp_cpu.hi = (uint32_t)((uint64_t)r >> 32);
}

static inline void psp_multu(uint32_t a, uint32_t b) {
    uint64_t r = (uint64_t)a * (uint64_t)b;
    psp_cpu.lo = (uint32_t)r;
    psp_cpu.hi = (uint32_t)(r >> 32);
}

static inline void psp_div(uint32_t a, uint32_t b) {
    int32_t sa = (int32_t)a, sb = (int32_t)b;
    if (sb == 0) {
        psp_cpu.lo = sa < 0 ? 1u : 0xFFFFFFFFu;
        psp_cpu.hi = a;
    } else if (sa == (int32_t)0x80000000 && sb == -1) {
        psp_cpu.lo = 0x80000000u;
        psp_cpu.hi = 0;
    } else {
        psp_cpu.lo = (uint32_t)(sa / sb);
        psp_cpu.hi = (uint32_t)(sa % sb);
    }
}

static inline void psp_divu(uint32_t a, uint32_t b) {
    if (b == 0) {
        psp_cpu.lo = 0xFFFFFFFFu;
        psp_cpu.hi = a;
    } else {
        psp_cpu.lo = a / b;
        psp_cpu.hi = a % b;
    }
}

static inline void psp_madd(uint32_t a, uint32_t b) {
    int64_t acc = (int64_t)(((uint64_t)psp_cpu.hi << 32) | psp_cpu.lo);
    acc += (int64_t)(int32_t)a * (int64_t)(int32_t)b;
    psp_cpu.lo = (uint32_t)acc;
    psp_cpu.hi = (uint32_t)((uint64_t)acc >> 32);
}

static inline void psp_maddu(uint32_t a, uint32_t b) {
    uint64_t acc = ((uint64_t)psp_cpu.hi << 32) | psp_cpu.lo;
    acc += (uint64_t)a * (uint64_t)b;
    psp_cpu.lo = (uint32_t)acc;
    psp_cpu.hi = (uint32_t)(acc >> 32);
}

static inline void psp_msub(uint32_t a, uint32_t b) {
    int64_t acc = (int64_t)(((uint64_t)psp_cpu.hi << 32) | psp_cpu.lo);
    acc -= (int64_t)(int32_t)a * (int64_t)(int32_t)b;
    psp_cpu.lo = (uint32_t)acc;
    psp_cpu.hi = (uint32_t)((uint64_t)acc >> 32);
}

static inline void psp_msubu(uint32_t a, uint32_t b) {
    uint64_t acc = ((uint64_t)psp_cpu.hi << 32) | psp_cpu.lo;
    acc -= (uint64_t)a * (uint64_t)b;
    psp_cpu.lo = (uint32_t)acc;
    psp_cpu.hi = (uint32_t)(acc >> 32);
}

static inline uint32_t psp_clz(uint32_t v) {
    if (v == 0) return 32;
    uint32_t n = 0;
    while (!(v & 0x80000000u)) { v <<= 1; n++; }
    return n;
}

static inline uint32_t psp_clo(uint32_t v) { return psp_clz(~v); }

static inline uint32_t psp_ext(uint32_t v, uint32_t pos, uint32_t size) {
    if (size == 0 || size > 32) return 0;
    if (size == 32) return v >> pos;
    return (v >> pos) & ((1u << size) - 1);
}

static inline uint32_t psp_ins(uint32_t dst, uint32_t src, uint32_t pos, uint32_t size) {
    if (size == 0 || size > 32) return dst;
    uint32_t mask = (size == 32) ? 0xFFFFFFFFu : (((1u << size) - 1) << pos);
    return (dst & ~mask) | ((src << pos) & mask);
}

static inline uint32_t psp_seb(uint32_t v) { return (uint32_t)(int32_t)(int8_t)(v & 0xFF); }
static inline uint32_t psp_seh(uint32_t v) { return (uint32_t)(int32_t)(int16_t)(v & 0xFFFF); }

static inline uint32_t psp_wsbh(uint32_t v) {
    return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
}
static inline uint32_t psp_wsbw(uint32_t v) {
    return (v << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8) | (v >> 24);
}

static inline uint32_t psp_bitrev(uint32_t v) {
    v = ((v & 0x55555555u) << 1)  | ((v >> 1)  & 0x55555555u);
    v = ((v & 0x33333333u) << 2)  | ((v >> 2)  & 0x33333333u);
    v = ((v & 0x0F0F0F0Fu) << 4)  | ((v >> 4)  & 0x0F0F0F0Fu);
    v = ((v & 0x00FF00FFu) << 8)  | ((v >> 8)  & 0x00FF00FFu);
    return (v << 16) | (v >> 16);
}

static const uint32_t PSP_LWL_MASK [4] = { 0x00FFFFFFu, 0x0000FFFFu, 0x000000FFu, 0x00000000u };
static const uint32_t PSP_LWL_SHIFT[4] = { 24, 16, 8, 0 };
static const uint32_t PSP_LWR_MASK [4] = { 0x00000000u, 0xFF000000u, 0xFFFF0000u, 0xFFFFFF00u };
static const uint32_t PSP_LWR_SHIFT[4] = { 0, 8, 16, 24 };
static const uint32_t PSP_SWL_MASK [4] = { 0xFFFFFF00u, 0xFFFF0000u, 0xFF000000u, 0x00000000u };
static const uint32_t PSP_SWL_SHIFT[4] = { 24, 16, 8, 0 };
static const uint32_t PSP_SWR_MASK [4] = { 0x00000000u, 0x000000FFu, 0x0000FFFFu, 0x00FFFFFFu };
static const uint32_t PSP_SWR_SHIFT[4] = { 0, 8, 16, 24 };

static inline uint32_t psp_lwl(uint32_t old, uint32_t addr) {
    uint32_t b = addr & 3;
    uint32_t w = psp_read32(addr & ~3u);
    return (old & PSP_LWL_MASK[b]) | (w << PSP_LWL_SHIFT[b]);
}
static inline uint32_t psp_lwr(uint32_t old, uint32_t addr) {
    uint32_t b = addr & 3;
    uint32_t w = psp_read32(addr & ~3u);
    return (old & PSP_LWR_MASK[b]) | (w >> PSP_LWR_SHIFT[b]);
}
static inline void psp_swl(uint32_t val, uint32_t addr) {
    uint32_t b = addr & 3, base = addr & ~3u;
    psp_write32(base, (psp_read32(base) & PSP_SWL_MASK[b]) | (val >> PSP_SWL_SHIFT[b]));
}
static inline void psp_swr(uint32_t val, uint32_t addr) {
    uint32_t b = addr & 3, base = addr & ~3u;
    psp_write32(base, (psp_read32(base) & PSP_SWR_MASK[b]) | (val << PSP_SWR_SHIFT[b]));
}

static inline float psp_bits_to_f32(uint32_t b) {
    union { uint32_t u; float f; } c;
    c.u = b;
    return c.f;
}
static inline uint32_t psp_f32_to_bits(float v) {
    union { uint32_t u; float f; } c;
    c.f = v;
    return c.u;
}

static inline float psp_fabs(float v)  { return v < 0.0f ? -v : v; }
static inline float psp_fsqrt(float v) {
    if (v <= 0.0f) return 0.0f;
    float g = v;
    for (int i = 0; i < 24; i++) g = 0.5f * (g + v / g);
    return g;
}

static inline int psp_fcmp(unsigned cond, float a, float b) {
    const int unordered = (a != a) || (b != b);
    switch (cond & 0xF) {
    case 0x0: case 0x8: return 0;
    case 0x1: case 0x9: return unordered;
    case 0x2: case 0xA: return !unordered && a == b;
    case 0x3: case 0xB: return unordered || a == b;
    case 0x4: case 0xC: return !unordered && a <  b;
    case 0x5: case 0xD: return unordered || a <  b;
    case 0x6: case 0xE: return !unordered && a <= b;
    default:            return unordered || a <= b;
    }
}

void psp_syscall(uint32_t id);
void psp_unimplemented(uint32_t addr, const char *what);
uint64_t psp_syscall_total(void);
uint64_t psp_unimplemented_total(void);
void psp_trap_reset(void);

/* ======================================================================
 * dispatch.h
 * ==================================================================== */
typedef void (*psp_fn_t)(void);

void psp_register(uint32_t addr, psp_fn_t fn);
void psp_register_label(uint32_t addr, psp_fn_t fn);
void psp_dispatch_set_budget(uint64_t calls, void (*on_exceeded)(void));
uint64_t psp_dispatch_calls(void);

psp_fn_t psp_lookup(uint32_t addr);
void psp_dispatch(uint32_t addr);
void psp_call_from_host(uint32_t addr);
int  psp_ra_is_jump(uint32_t ra);

typedef void (*psp_miss_fn_t)(uint32_t addr);
void psp_set_miss_handler(psp_miss_fn_t fn);

uint32_t psp_dispatch_count(void);
uint64_t psp_dispatch_misses(void);
uint64_t psp_dispatch_last(void);
uint32_t psp_dispatch_low_calls(void);      /* indirect calls into 0x000..0xFFF */
uint32_t psp_dispatch_low_addr(int i);
uint32_t psp_dispatch_ctor_hits(void);     /* .cplinit entries actually called */
void psp_dispatch_reset(void);

/* Stubs generated by build.sh for symbols a partial link leaves undefined. */
void psp_stub_hit(uint32_t addr);
uint64_t psp_stub_calls(void);

/* The register routine emitted by allegrexrecomp (djmax_register.c), or the
 * subset equivalent build.sh generates. */
void psp_recomp_register(void);

/* ======================================================================
 * vfpu.h  (only the entry points this corpus references)
 * ==================================================================== */
int  psp_vfpu_regs(uint32_t vreg, int size, int out[4]);

void psp_lv_q(uint32_t vt, uint32_t addr);
void psp_sv_s(uint32_t vt, uint32_t addr);

void psp_vmul(uint32_t vd, uint32_t vs, uint32_t vt, int size);
void psp_vmin(uint32_t vd, uint32_t vs, uint32_t vt, int size);
void psp_vsge(uint32_t vd, uint32_t vs, uint32_t vt, int size);
void psp_vscl(uint32_t vd, uint32_t vs, uint32_t vt, int size);
void psp_vhdp(uint32_t vd, uint32_t vs, uint32_t vt, int size);
void psp_vtfm(uint32_t vd, uint32_t vs, uint32_t vt, int size);
void psp_vmmul(uint32_t vd, uint32_t vs, uint32_t vt, int size);
void psp_vcmp(uint32_t cond, uint32_t vs, uint32_t vt, int size);

/* ======================================================================
 * hle.h  (counters only -- every import returns 0)
 * ==================================================================== */
typedef void (*psp_hle_fn)(void);

void psp_hle_register(uint32_t nid, const char *lib, const char *name, psp_hle_fn fn);
void psp_hle_register_unnamed(uint32_t nid, const char *lib, psp_hle_fn fn);
void psp_hle_call(uint32_t nid);

const char *psp_hle_name(uint32_t nid);
int         psp_hle_count(void);
void        psp_hle_init(void);

uint64_t psp_hle_unimplemented_calls(void);
uint64_t psp_hle_total_calls(void);
int      psp_hle_distinct(void);
uint32_t psp_hle_calls(uint32_t nid);
uint32_t psp_hle_last_nid(int back);   /* 0 = most recent */
uint32_t psp_hle_last_ra(void);      /* guest RA of the most recent import */
void     psp_hle_dump_ring(void);   /* last 32 imports: nid, args, ret, ra */
const char *psp_hle_name_of(uint32_t nid);
uint64_t psp_hle_zero_calls(void);
void     psp_hle_dump_counts(int limit);
void     psp_hle_unimplemented_reset(void);

/* ---- console output ----------------------------------------------------
 * ps2sdk's printf goes through IOP FILEIO and produces nothing in a plain
 * PCSX2 boot (verified: hello.elf's printf never reaches emulog.txt). A byte
 * written to the EE TTY register does, so the runner uses this instead. */
void psp_tty_write(const char *s);
void psp_watch(uint32_t site);   /* selective guest instrumentation (debug) */
void psp_dbg_readret(void);      /* return hook for sceIoReadAsync */
void psp_dbg_call74724(void);    /* entry hook for the wrapper that reads TOC */
void psp_printf(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* DJMX_PS2_RUNTIME_MIN_H */
