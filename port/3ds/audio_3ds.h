/* audio_3ds.h -- salida de audio real (ndsp) para los canales sceAudio del PSP. */
#ifndef DJMX_3DS_AUDIO_H
#define DJMX_3DS_AUDIO_H

#include <stdint.h>

#define PS2_AUDIO_CH 8          /* canales de usuario del PSP */

void ps2_audio_install(void);   /* arranca ndsp; si falta dspfirm crea uno dummy */
void ps2_audio_fini(void);
int  ps2_audio_ok(void);

/* FIFO por canal, en frames estéreo s16 a 44100 Hz. */
uint32_t ps2_audio_level(int ch);                              /* frames en cola */
uint32_t ps2_audio_underruns(int ch);                          /* callbacks sin trozo entero */
void     ps2_audio_push(int ch, const int16_t *lr, uint32_t frames);
void     ps2_audio_reset(int ch);

uint64_t ps2_time_us(void);

/* Pacing de audio fiel al PSP: esperas con fecha límite y preempción desde
 * psp_hle_call para que los hilos de audio respiren a mitad de frame. */
void ps2_audio_wait_until(uint64_t when_us);
void ps2_audio_preempt(void);
void ps2_audio_set_pump(void (*fn)(void));

#endif
