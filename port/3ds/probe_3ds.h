/* probe_3ds.h -- per-instruction probe for the instrumented corpus only.
 *
 * build/ps2/src carries one PSP_I(0xADDR) per emitted guest instruction. The
 * clean corpus in build/emit does not, so this header is force-included only
 * with build.ps1 -Instrumented; it turns every site into a call into the
 * runtime, which counts retired guest instructions exactly.
 */
#ifndef DJMX_3DS_PROBE_H
#define DJMX_3DS_PROBE_H

#include <stdint.h>

extern uint64_t psp_host_icount;
extern uint32_t psp_host_pc;

void psp_host_i(uint32_t addr);

#define PSP_I(a) psp_host_i(a)

#endif
