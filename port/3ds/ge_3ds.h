/* ge_3ds.h -- minimal GU/GE backend for the 3DS port.
 *
 * The PS2 port's ps2_ge.c parses the whole GS display list and rasterises it.
 * That parser is architecture independent; this build starts with the counter
 * backend (lists enqueued, frame buffers swapped) so the boot can be measured
 * before the renderer is moved over. */
#ifndef DJMX_3DS_GE_H
#define DJMX_3DS_GE_H

#include <stdint.h>

void     ps2_ge_init(void);
void     ps2_ge_selftest(void);
int      ps2_ge_enqueue(uint32_t list, uint32_t stall);
void     ps2_ge_update_stall(uint32_t old_stall, uint32_t new_stall);
void     ps2_ge_frame(uint32_t fb, uint32_t sync);
uint32_t ps2_ge_frames_drawn(void);
uint32_t ps2_ge_list_count(void);
uint32_t ps2_ge_prim_count(void);
uint32_t ps2_ge_vtype_count(void);
uint32_t ps2_ge_vtype_nonzero(void);
uint32_t ps2_ge_fb_addr(void);
uint32_t ps2_ge_fps_x100(void);
void     ps2_ge_dump_request(void);
void     ps2_ge_vram_copy(uint32_t dst, uint32_t src, uint32_t n);

#endif
