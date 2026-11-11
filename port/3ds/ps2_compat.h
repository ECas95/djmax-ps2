/* ps2_compat.h -- the small piece of ps2sdk surface ps2_hle.c used.
 *
 * On the 3DS there is no SIF, no IOP modules and no PADMAN, so these are
 * counters that report "absent". The HLE code paths that use them are the
 * controller and the optional autopad file; neither blocks the boot.
 */
#ifndef DJMX_3DS_PS2_COMPAT_H
#define DJMX_3DS_PS2_COMPAT_H

#include <stdint.h>

/* SIF / IOP module loading: nothing to load on the 3DS. */
int SifInitRpc(unsigned int mode);
int SifLoadModule(const char *path, int arg_len, const char *args);

/* PADMAN. Only the names and layout ps2_hle.c touches. */
typedef struct padButtonStatus {
    uint16_t btns;
    uint8_t  ljoy_h, ljoy_v;
    uint8_t  rjoy_h, rjoy_v;
    uint8_t  mode;
} padButtonStatus;

/* audio_3ds.c: guest-visible microseconds from the ARM11 system tick. */
uint64_t ps2_time_us(void);

#define PAD_STATE_DISCONN 0
#define PAD_STATE_STABLE  1
#define PAD_STATE_FINDCTP1 2
#define PAD_MMODE_DUALSHOCK 0x7
#define PAD_MMODE_LOCK      0

#define PAD_L1 0x0400
#define PAD_L2 0x0100
#define PAD_R1 0x0800
#define PAD_R2 0x0200
#define PAD_R3 0x0004

int  padInit(int mode);
int  padPortOpen(int port, int slot, void *padbuf);
int  padGetState(int port, int slot);
int  padRead(int port, int slot, padButtonStatus *data);
int  padSetMainMode(int port, int slot, int mode, int lock);

#endif
