/* video_3ds.h -- top screen 400x240 (guest framebuffer) + bottom console. */
#ifndef DJMX_3DS_VIDEO_H
#define DJMX_3DS_VIDEO_H

#include <stdint.h>

void video_init(void);
void video_shutdown(void);

/* Copy a guest PSP framebuffer (480x272, 16 bpp, 512-byte stride) to the top
 * screen, scaled to 400x240. No-op while no frame buffer has been set. */
void video_present(uint32_t guest_fb);

/* Console sink used by psp_printf (runtime_3ds.c): bottom screen + sdmc log. */
void psp_console_write(const char *s);

uint32_t video_frames(void);
int  video_log_ok(void);

/* Citro3D owns the top screen when active. Never overwrite it with gfx framebuffer writes. */
void *video_pica_target(void);
int video_pica_ready(void);
#endif
