/* ps2_hle.h -- firmware handlers for the PS2 port. */
#ifndef DJMX_PS2_HLE_H
#define DJMX_PS2_HLE_H

#include <stdint.h>

void ps2_hle_install(void);
void ps2_pad_init(void);

void     ps2_sysmem_init(void);
uint32_t ps2_sysmem_alloc(uint32_t size, uint32_t type, uint32_t addr_hint);
int      ps2_sysmem_free_block(uint32_t uid);
uint32_t ps2_sysmem_head(uint32_t uid);
uint32_t ps2_sysmem_total_free(void);
uint32_t ps2_sysmem_max_free(void);
uint32_t ps2_stack_alloc(uint32_t size);   /* thread stacks: top of guest RAM, outside the heap */

#endif /* DJMX_PS2_HLE_H */
