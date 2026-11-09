#ifndef DJMX_PS2_BGA_H
#define DJMX_PS2_BGA_H
#include <stdint.h>
void ps2_bga_audio_reset(void);
void ps2_bga_audio_seek(uint32_t sample);
void ps2_bga_audio_samples(uint32_t samples);
uint32_t ps2_bga_texture(uint8_t *pixels, uint8_t *palette);
void ps2_bga_frame(void);
#endif
