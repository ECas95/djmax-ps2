/* ps2_ge.h -- PSP GE display-list walker. */
#ifndef DJMX_PS2_GE_H
#define DJMX_PS2_GE_H

#include <stdint.h>

uint32_t ps2_ge_enqueue(uint32_t list, uint32_t stall);   /* returns the queue id */
void     ps2_ge_update_stall(uint32_t id, uint32_t stall);
void     ps2_ge_init(void);
void     ps2_ge_frame(uint32_t fb, uint32_t sync);        /* sceDisplaySetFrameBuf(fb, .., .., sync) */
uint32_t ps2_ge_frames_drawn(void);
void     ps2_ge_vram_copy(uint32_t dst, uint32_t src, uint32_t size);   /* sceDmacMemcpy into VRAM */
uint32_t ps2_ge_vertex_size(uint32_t vtype);
void     ps2_ge_dump_request(void);                       /* TEMP: log the next frame */

#endif /* DJMX_PS2_GE_H */
