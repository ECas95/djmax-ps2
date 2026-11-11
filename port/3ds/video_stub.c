/* video_stub.c -- replaces the game's blocking movie player (0x8B230).
 * Both callers ignore its return value; the real video path will come from
 * the MJPEG route, so here it simply returns 0. */
#include "runtime_3ds.h"

/* Returning early leaves the player's audio thread unstarted and the boot hangs
 * (3 frames); the real pump exits cleanly through the HLE's GetAtracAu EOS. */
#ifndef DJMX_SKIP_MOVIE_PUMP
#define DJMX_SKIP_MOVIE_PUMP 0
#endif

void psp_real_0008B230(void);

void psp_func_0008B230(void) {
    static int n;
    if (!DJMX_SKIP_MOVIE_PUMP) {
        if (n++ < 2) psp_printf("[movie] pump 0x8B230 (EOS path)\n");
        psp_real_0008B230();
        return;
    }
    if (n++ < 2) psp_printf("[stub] movie player 0x8B230 -> 0\n");
    psp_ret(0);
}