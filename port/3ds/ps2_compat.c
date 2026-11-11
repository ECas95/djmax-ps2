/* ps2_compat.c -- see ps2_compat.h.
 * El pad "PS2" se sirve con el HID real del 3DS: mismo layout de bits que el
 * PSP, así pad_buttons() (hle_3ds.c) funciona sin cambios.
 *   3DS A/B/X/Y -> PSP CIRCLE/CROSS/TRIANGLE/SQUARE; L/R -> L/R;
 *   d-pad -> d-pad; START/SELECT -> START/SELECT; circle pad -> nub analógico. */
#include <3ds.h>
#include "ps2_compat.h"

int SifInitRpc(unsigned int mode) { (void)mode; return 0; }
int SifLoadModule(const char *path, int arg_len, const char *args) {
    (void)path; (void)arg_len; (void)args;
    return 0;                     /* fingido: el "PADMAN" es el HID del 3DS */
}

int padInit(int mode) { (void)mode; return 1; }
int padPortOpen(int port, int slot, void *padbuf) { (void)port; (void)slot; (void)padbuf; return 1; }
int padGetState(int port, int slot) { (void)port; (void)slot; return PAD_STATE_STABLE; }
int padRead(int port, int slot, padButtonStatus *data) {
    (void)port; (void)slot;
    hidScanInput();
    const u32 k = hidKeysHeld();
    /* SELECT en gameplay cicla el layout de pantallas (no llega al juego). */
    extern int ge_draw_split_active(void);
    extern void ge_draw_cycle_layout(void);
    static u32 prev;
    const u32 kd = k & ~prev;
    prev = k;
    const int in_game = ge_draw_split_active();
    if (in_game && (kd & KEY_SELECT)) ge_draw_cycle_layout();
    uint16_t on = 0;
    if ((k & KEY_SELECT) && !in_game) on |= 0x0001u;
    if (k & KEY_START)  on |= 0x0008u;
    if (k & KEY_DUP)    on |= 0x0010u;
    if (k & KEY_DRIGHT) on |= 0x0020u;
    if (k & KEY_DDOWN)  on |= 0x0040u;
    if (k & KEY_DLEFT)  on |= 0x0080u;
    if (k & (KEY_L | KEY_ZL)) on |= PAD_L1;
    if (k & (KEY_R | KEY_ZR)) on |= PAD_R1;
    if (k & KEY_X) on |= 0x1000u;   /* TRIANGLE */
    if (k & KEY_A) on |= 0x2000u;   /* CIRCLE */
    if (k & KEY_B) on |= 0x4000u;   /* CROSS */
    if (k & KEY_Y) on |= 0x8000u;   /* SQUARE */
    data->btns = (uint16_t)(0xFFFFu ^ on);      /* activo a nivel bajo */
    circlePosition cp;
    hidCircleRead(&cp);
    int h = 128 + ((int)cp.dx * 127) / 156;
    int v = 128 - ((int)cp.dy * 127) / 156;
    if (h < 0) h = 0; else if (h > 255) h = 255;
    if (v < 0) v = 0; else if (v > 255) v = 255;
    data->ljoy_h = (uint8_t)h;
    data->ljoy_v = (uint8_t)v;
    data->rjoy_h = data->rjoy_v = 128;
    data->mode = 0x73;                          /* analógico */
    return 1;
}
int padSetMainMode(int port, int slot, int mode, int lock) {
    (void)port; (void)slot; (void)mode; (void)lock;
    return 1;
}
