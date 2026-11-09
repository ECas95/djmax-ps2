/* ps2_snd.h -- mix of the PSP audio channels, out through audsrv (ps2_snd.c).
 * Times are absolute sample indices at 44.1 kHz (ps2_audio.c converts from EE cycles). */
#ifndef DJMX_PS2_SND_H
#define DJMX_PS2_SND_H

#include <stdint.h>

/* Calibration knob: samples behind now before a mixed sample is final and goes out (35 ms).
 * Covers an output call that runs a game frame late (cooperative scheduler). */
#define SND_LAG 1536u

void ps2_snd_init(void);   /* load libsd + audsrv on the IOP (boot, main thread) */

/* Add n frames of s16 PCM (interleaved if stereo) starting at sample `at`; volumes 0..0x8000. */
void ps2_snd_mix(uint64_t at, const int16_t *pcm, uint32_t n, int stereo,
                 int32_t lvol, int32_t rvol, uint64_t now);
/* Main thread, once per frame: writes the debug dump, prints status every 10 s. */
void ps2_snd_report(uint64_t now);
/* From the vblank interrupt: wakes the thread that sends the mix to the SPU2. */
void ps2_snd_vblank_isr(void);
/* Estimated sample currently consumed by audsrv, in the output mix timeline. */
uint64_t ps2_snd_playhead(void);

#endif
