/* main.c -- DJMAX Fever on the Nintendo 3DS: boot the recompiled PSP module.
 *
 * Boot sequence:
 *   1. libctru video/console and the guest memory windows (module 0..5.5 MB,
 *      RAM 0x08000000..0x09800000);
 *   2. copy the EBOOT's two PT_LOAD segments (embedded at build time) so the
 *      absolute guest addresses in the recompiled C resolve as on the PSP;
 *   3. register the 31k translated entries and 208 import thunks;
 *   4. install the HLE and the cooperative scheduler;
 *   5. call module_start (0x91974), then pump guest threads until they block,
 *      die, or a function-entry budget stops the run;
 *   6. print the report (PC, entries, NIDs, dispatch) and exit.
 */
#include <3ds.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "runtime_3ds.h"
#include "sched_3ds.h"
#include "hle_3ds.h"
#include "ge_3ds.h"
#include "ge_draw.h"
#include "video_3ds.h"
#include "audio_3ds.h"

/* Built by build.ps1: extracted PT_LOAD segments of build/eboot_reloc.bin. */
extern const unsigned char seg0_start[];
extern const unsigned char seg0_end[];
extern const unsigned char seg1_start[];
extern const unsigned char seg1_end[];

void ps2_imports_register(void);

#define SEG0_VADDR   0x00000000u
#define SEG1_VADDR   0x000E15D8u
#define SEG1_BSS     0x0045BCDCu
#define ENTRY_ADDR   0x00091974u

/* Function-entry budget: 0 = sin límite (juego completo). Un valor finito
 * sirve para corridas de diagnóstico que deben terminar solas. */
#define ENTRY_BUDGET  0ull
#define HLE_BUDGET    0ull

static jmp_buf g_stop;
static volatile int g_reason;          /* 1 entries, 2 HLE, 3 exit */

void psp_stop_now(void) { g_reason = 2; longjmp(g_stop, 1); }
static void entries_hit(void) { g_reason = 1; longjmp(g_stop, 1); }
static void dispatch_hit(void) { g_reason = 1; longjmp(g_stop, 1); }

static void report(const char *why) {
    psp_printf("--- report (%s) ---\n", why);
    psp_printf("pc            : 0x%08X\n", psp_last_enter);
    if (psp_host_icount)
        psp_printf("instructions  : %llu\n", (unsigned long long)psp_host_icount);
    else
        psp_printf("entries       : %llu translated bodies (no per-insn probe)\n",
                   (unsigned long long)psp_func_entries);
    psp_printf("hle           : %d NIDs, %llu calls, %llu unimplemented\n",
               psp_hle_distinct(), (unsigned long long)psp_hle_total_calls(),
               (unsigned long long)psp_hle_unimplemented_calls());
    psp_printf("dispatch      : %u entries, %llu misses\n",
               psp_dispatch_count(), (unsigned long long)psp_dispatch_misses());
    psp_printf("memory        : %llu bad accesses\n",
               (unsigned long long)psp_mem_bad_access);
    psp_printf("threads       : %d created, %d live, %d ready\n",
               ps2_sched_created(), ps2_sched_live(), ps2_sched_ready());
    psp_printf("ge            : %u lists, %u frames, %u PRIM, %u VTYPE (%u != 0)\n",
               ps2_ge_list_count(), ps2_ge_frames_drawn(), ps2_ge_prim_count(),
               ps2_ge_vtype_count(), ps2_ge_vtype_nonzero());
    psp_printf("fb            : 0x%08X   fps(emulado)=%u.%02u\n",
               ps2_ge_fb_addr(), ps2_ge_fps_x100() / 100u, ps2_ge_fps_x100() % 100u);
    psp_printf("last NIDs     :");
    for (int k = 7; k >= 0; k--) {
        const uint32_t n = psp_hle_last_nid(k);
        if (n) psp_printf(" %08X", n);
    }
    psp_printf("\n");
    psp_printf("unimplemented NIDs:\n");
    psp_hle_dump_unimplemented();
    psp_printf("top NIDs:\n");
    psp_hle_dump_counts(10);
    ps2_sched_dump_states();
    ps2_sched_dump_stacks();
    psp_printf("=== end ===\n");
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    /* New 3DS: 804 MHz + caché L2 (no-op en Old 3DS). El recomp es CPU-bound:
     * en Old 3DS el juego arranca pero no sostiene los 30 fps. */
    osSetSpeedupEnable(true);

    video_init();
    psp_printf("\n=== DJMAX Fever / 3DS: guest module + cooperative scheduler ===\n");
    {
        bool n3ds = false;
        APT_CheckNew3DS(&n3ds);
        psp_printf("[sys] New3DS=%d speedup=on\n", (int)n3ds);
    }

    psp_mem_init();
    const uint32_t seg0_len = (uint32_t)(seg0_end - seg0_start);
    const uint32_t seg1_len = (uint32_t)(seg1_end - seg1_start);
    psp_printf("seg0 %u B @0x%08X, seg1 %u B @0x%08X, bss %u B\n",
               seg0_len, SEG0_VADDR, seg1_len, SEG1_VADDR, SEG1_BSS);
    if (psp_mem_load_module(seg0_start, seg0_len, seg1_start, seg1_len,
                            SEG1_VADDR, SEG1_BSS) != 0)
        psp_printf("FATAL: segment copy failed\n");
    psp_printf("after load  : [0x0]=0x%08X [0x%08X]=0x%08X\n",
               psp_read32(0), SEG1_VADDR, psp_read32(SEG1_VADDR));

    psp_cpu_reset();
    psp_recomp_register();
    ps2_imports_register();
    psp_printf("dispatch    : %u entries\n", psp_dispatch_count());

    ps2_sysmem_init();
    ps2_sched_init();
    psp_hle_init();
    ps2_audio_install();
    ps2_ge_init();
    /* Create sdmc:/djmx_selftest.flag to request an isolated PICA geometry test. */
    FILE *selftest_flag = fopen("sdmc:/djmx_selftest.flag", "rb");
    if (selftest_flag) {
        fclose(selftest_flag);
        ps2_ge_selftest();
    }
    ps2_pad_init();

    psp_cpu.r[PSP_REG_SP] = PSP_RAM_BASE + PSP_RAM_SIZE - 0x100u;
    psp_cpu.r[PSP_REG_GP] = 0;
    psp_cpu.r[PSP_REG_RA] = 0;

    psp_func_exceeded = entries_hit;
    psp_func_limit = ENTRY_BUDGET;
    psp_host_exceeded = entries_hit;
    psp_host_limit = ENTRY_BUDGET;
    psp_dispatch_set_budget(HLE_BUDGET, dispatch_hit);

    psp_printf("entry       : 0x%08X, budget %llu bodies\n",
               ENTRY_ADDR, (unsigned long long)ENTRY_BUDGET);

    if (setjmp(g_stop) == 0) {
        psp_call_from_host(ENTRY_ADDR);
        psp_printf("module_start returned v0=0x%08X after %llu bodies\n",
                   psp_cpu.r[PSP_REG_V0], (unsigned long long)psp_func_entries);
        psp_printf("threads     : created=%d live=%d ready=%d\n",
                   ps2_sched_created(), ps2_sched_live(), ps2_sched_ready());
        const int sw = ps2_sched_pump();
        psp_printf("scheduler drained after %d switches, %llu bodies\n",
                   sw, (unsigned long long)psp_func_entries);
        report("threads blocked or dead");
    } else {
        report(g_reason == 1 ? "function-entry budget" :
               g_reason == 2 ? "HLE call budget" : "stopped");
    }

    /* Keep the window alive long enough to read it; START exits immediately. */
    psp_printf("press START to exit\n");
    for (int i = 0; i < 600; i++) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
        if (i == 60) psp_printf("(still alive)\n");
    }
    ge_draw_fini();
    ps2_audio_fini();
    video_shutdown();
    return 0;
}
