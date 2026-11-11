/* ge_draw.h -- PSP GE display-list interpreter and PICA200 (citro3d) renderer.
 *
 * Contract with the runtime (see port/3ds/ge_3ds.c, which this module does not
 * modify):
 *
 *   ge_draw_init()                      once, after C3D_Init()
 *   ge_draw_list(list, stall)           from ps2_ge_enqueue()
 *   ge_draw_frame(fb, sync)             from ps2_ge_frame(): closes the PICA frame
 *   ge_draw_present()                   optional explicit present hook
 *
 * Every guest frame is one citro3d frame: ge_draw_list() opens it lazily on the
 * first draw and ge_draw_frame() ends it, so the C3D command buffer never grows
 * past one guest frame.
 *
 * Opcodes handled: 0x01 VADDR, 0x02 IADDR, 0x04 PRIM, 0x08 JUMP, 0x0A CALL,
 * 0x0B RET, 0x0C END, 0x0E SIGNAL, 0x0F FINISH, 0x10 BASE, 0x12 VTYPE,
 * 0x13 OFFSET, 0x14 ORIGIN, 0x3A..0x41 matrices, 0xA0 TEXADDR0, 0xA8
 * TEXBUFWIDTH0, 0xB0/B1 CLUTADDR, 0xB8 TEXSIZE0, 0xC2 TEXMODE, 0xC3 TEXFORMAT,
 * 0xC5 TEXCLUT, 0xD4/D5 scissor, 0x42..0x47 viewport. Everything else is
 * counted as an ignored command, never fatal.
 */
#ifndef DJMX_3DS_GE_DRAW_H
#define DJMX_3DS_GE_DRAW_H

#include <stdint.h>
#include <stddef.h>

#define GE_CENSUS_MAX 120

typedef struct {
    uint32_t list_pc, prim_pc;
    uint32_t type, count;
    uint32_t vtype;
    uint32_t vaddr, stride;
    uint32_t pos_type, uv_type, color_type, index_type, through_2d;
    uint32_t tex_addr, tex_w, tex_h, tex_psm, tex_on, tex_stride;
    int32_t  bbox[4];              /* screen px: minx, miny, maxx, maxy */
    const char *status;            /* SUBMITTED, NO_POSITION, ... */
} ge_census_call;

void ge_draw_init(void);
/* PSP sceGeListEnQueue returns a list ID. UpdateStallAddr must resume the
 * SAME program counter/call stack, not reparse from the start. */
typedef struct {
    uint32_t pc, stall;
    uint32_t ret[32], saved_offset[32];
    uint8_t sp, finished;
} ge_draw_list_state;
void ge_draw_start(ge_draw_list_state *s, uint32_t list, uint32_t stall);
/* 1: finished, 0: stalled, -1: invalid list or budget exhausted. */
int ge_draw_resume(ge_draw_list_state *s);
void ge_draw_fini(void);
uint32_t ge_draw_vtypes(void);
uint32_t ge_draw_vtypes_nonzero(void);
int ge_draw_ready(void);

/* Optional: where ge_draw_list() draws. `target` is a C3D_RenderTarget*.
 * Without one, geometry is decoded/censused but not rasterised. */
void ge_draw_set_target(void *target);
void ge_draw_set_bottom(void *target);
void ge_draw_set_clear(uint32_t rgba);   /* 0xRRGGBBAA, default 0x000000FF */

/* Interprets one GE list, drawing into the current PICA frame.
 * Returns the number of PRIM commands seen. */
uint32_t ge_draw_list(uint32_t list, uint32_t stall);

/* Called from ps2_ge_frame(): ends the PICA frame. */
void ge_draw_frame(uint32_t fb, uint32_t sync);

/* Draws a frame's worth of already-parsed geometry (no-op: drawing is immediate). */
void ge_draw_present(void);

/* Census / diagnostics. */
uint32_t ge_draw_prims(void);
uint32_t ge_draw_submitted(void);
uint32_t ge_draw_rejected_no_pos(void);
uint32_t ge_draw_rejected_bad_addr(void);
uint32_t ge_draw_rejected_other(void);
uint32_t ge_draw_lists(void);
uint32_t ge_draw_cmds(void);
uint32_t ge_draw_frames(void);
uint32_t ge_draw_textured(void);
/* Texels: prims with texture enabled that were submitted; miss == bind until a
 * texture cache exists. */
uint32_t ge_draw_tex_bind(void);
uint32_t ge_draw_tex_fail(void);
uint32_t ge_draw_tex_miss(void);
int      ge_draw_census_count(void);
const ge_census_call *ge_draw_census(int i);
void ge_draw_census_reset(void);

/* Formats the JSON census (first GE_CENSUS_MAX drawcalls + counters).
 * Returns the number of bytes that would have been written. */
int ge_draw_census_json(char *buf, size_t cap, uint32_t frame);

/* Diagnostic: opaque magenta instead of the texture (phase C). */
void ge_draw_print_hist(void);
void ge_draw_set_magenta(int on);
int  ge_draw_magenta(void);

#endif


