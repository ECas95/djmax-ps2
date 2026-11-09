/* ps2_audio.h -- real time, sceAudio, sceAtrac and sceSas, all silent. */
#ifndef DJMX_PS2_AUDIO_H
#define DJMX_PS2_AUDIO_H

#include <stdint.h>

/* Registers the handlers; call after ps2_hle_install (the last registration of a NID wins). */
void ps2_audio_install(void);

/* Microseconds since install, from the EE clock. Never wraps in practice. */
uint64_t ps2_time_us(void);

/* Optional call at the top of psp_hle_call: lets a thread whose delay has expired run
 * even if the current thread does not block, as the PSP's preemption would. */
void ps2_audio_preempt(void);

#endif /* DJMX_PS2_AUDIO_H */
