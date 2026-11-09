/* ps2_probe.h -- per-instruction probe for the PS2 build.
 *
 * The corpus compiled for the EE is the host-instrumented one (produced by
 * port/tools/make_corpus.ps1): every emitted guest instruction carries a
 * PSP_I(0xADDR) site. Force-included with -include, so the emitted C stays
 * untouched.
 *
 * Two modes:
 *   - PS2_TRACE not defined (default): PSP_I compiles to nothing. One `jal`
 *     per guest instruction at all 182 383 sites cost 2.8 MB of .text, and the
 *     EE does not have that room next to a 15.75 MB guest VPL.
 *   - PS2_TRACE defined: out-of-line call, counts retired instructions and the
 *     PC, prints progress every PROBE_REPORT and enforces psp_host_limit.
 */
#ifndef DJMX_PS2_PROBE_H
#define DJMX_PS2_PROBE_H

#include <stdint.h>

#define PROBE_REPORT 500000ull

extern uint64_t psp_host_icount;   /* guest instructions retired */
extern uint32_t psp_host_pc;       /* address of the last one */
extern uint64_t psp_host_limit;    /* hard stop */
extern uint64_t psp_host_next_report;

void psp_host_i(uint32_t a);        /* defined in main.c */
void psp_probe_tick(void);

#ifdef PS2_TRACE
#  define PSP_I(a) psp_host_i(a)
#else
#  define PSP_I(a) ((void)0)
#endif

#endif /* DJMX_PS2_PROBE_H */
