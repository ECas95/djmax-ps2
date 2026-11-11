/* DJMAX 3DS: PSP GE -> Citro3D backend.
 * Enqueue hands back a list ID, and UpdateStallAddr resumes that ID's PC.
 * Legacy 'scan 2048 words' counters were false positives in ordinary RAM.
 */
#include "ge_3ds.h"
#include "ge_draw.h"
#include "runtime_3ds.h"
#include "video_3ds.h"
#include <3ds.h>
#include <string.h>

#define GE_LIST_SLOTS 16u
typedef struct { ge_draw_list_state ge; uint8_t used; } GEListSlot;
static GEListSlot g_slots[GE_LIST_SLOTS];
static uint32_t g_lists, g_frames, g_fb, g_syncs, g_logged, g_bad_lists;
static uint64_t g_tick0, g_tick1;

void ps2_ge_init(void) {
    memset(g_slots, 0, sizeof g_slots);
    g_lists = g_frames = g_fb = g_syncs = g_logged = g_bad_lists = 0;
    g_tick0 = g_tick1 = 0;
    if (video_pica_ready()) {
        ge_draw_init();
        ge_draw_set_target(video_pica_target());
        ge_draw_set_magenta(0); /* Normal boot: test mode temporarily enables magenta. */
    }
    psp_printf("[GE] pica=%d shader=%d target=%p\n",
        video_pica_ready(), ge_draw_ready(), video_pica_target());
}

/* Synthetic fullscreen quad validates REAL PICA rendering separately from
 * guest assets, HLE, and PSP GE list scheduling. Opt-in flag on SD only. */
void ps2_ge_selftest(void) {
    if (!video_pica_ready() || !ge_draw_ready()) {
        psp_printf("[SELFTEST] FAIL: Citro3D or shader was not initialized\n");
        return;
    }
    const uint32_t list = PSP_RAM_BASE;
    const uint32_t va = list + 0x100u;
    psp_write32(list + 0x00u, 0x10080000u); /* BASE high nibble: 0x08 */
    psp_write32(list + 0x04u, 0x12800100u); /* 2D through, xyz short */
    psp_write32(list + 0x08u, 0x01000100u); /* VADDR = 0x08000100 */
    psp_write32(list + 0x0Cu, 0x04030006u); /* TRIANGLES, six vertices */
    psp_write32(list + 0x10u, 0x0C000000u); /* END */
    static const int16_t pts[6][3] = {
        {0,0,0}, {480,0,0}, {480,272,0},
        {0,0,0}, {480,272,0}, {0,272,0}
    };
    for (uint32_t i = 0; i < 6; i++)
        for (uint32_t k = 0; k < 3; k++)
            psp_write16(va + i * 6u + k * 2u, (uint16_t)pts[i][k]);
    const uint32_t prev = ge_draw_submitted();
    ge_draw_set_magenta(1);
    ge_draw_list(list, 0);
    ge_draw_present();
    ge_draw_set_magenta(0);
    psp_printf("[SELFTEST] quad_submitted=%u, expect MAGENTA top screen for 2 sec\n",
        ge_draw_submitted() - prev);
    for (int i = 0; i < 120; i++) gspWaitForVBlank();
    /* The game overwrites the temporary guest-RAM list when it boots. */
}

int ps2_ge_enqueue(uint32_t list, uint32_t stall) {
    for (uint32_t id = 0; id < GE_LIST_SLOTS; id++) {
        GEListSlot *slot = &g_slots[id];
        if (slot->used) continue;
        slot->used = 1;
        g_lists++;
        ge_draw_start(&slot->ge, list, stall);
        int rc = ge_draw_resume(&slot->ge);
        if (rc != 0) slot->used = 0;
        if (rc < 0) g_bad_lists++;
        if (g_logged++ < 20u) psp_printf("[GE] enqueue id=%u pc=%08X stall=%08X rc=%d real_PRIM=%u\n",
            id, list, stall, rc, ge_draw_prims());
        return (int)id;
    }
    static uint32_t warn;
    if (warn++ < 8u) psp_printf("[GE] ERROR: queue full, %u stalled slots\n", GE_LIST_SLOTS);
    return (int)0x80000021u;
}

void ps2_ge_update_stall(uint32_t id, uint32_t new_stall) {
    if (id >= GE_LIST_SLOTS || !g_slots[id].used) return;
    GEListSlot *slot = &g_slots[id];
    const uint32_t old_pc = slot->ge.pc;
    slot->ge.stall = new_stall;
    int rc = ge_draw_resume(&slot->ge);
    if (rc != 0) slot->used = 0;
    if (rc < 0) g_bad_lists++;
    static uint32_t logged;
    if (logged++ < 32u) psp_printf("[GE] resume id=%u pc=%08X->%08X stall=%08X rc=%d PRIM=%u\n",
        id, old_pc, slot->ge.pc, new_stall, rc, ge_draw_prims());
}

void ps2_ge_frame(uint32_t fb, uint32_t sync) {
    g_fb = fb;
    g_syncs += !!sync;
    g_frames++;
    if (g_frames == 1) g_tick0 = svcGetSystemTick();
    g_tick1 = svcGetSystemTick();
    if (video_pica_ready()) ge_draw_frame(fb, sync);
    if (g_frames <= 5 || (g_frames % 60u) == 0u) {
        uint32_t stalled = 0;
        for (uint32_t i = 0; i < GE_LIST_SLOTS; i++) stalled += g_slots[i].used;
        psp_printf("[GE] frame=%u lists=%u real_PRIM=%u submitted=%u no_pos=%u bad_addr=%u stalled=%u bad_lists=%u texb=%u texf=%u\n",
            g_frames, g_lists, ge_draw_prims(), ge_draw_submitted(),
            ge_draw_rejected_no_pos(), ge_draw_rejected_bad_addr(), stalled, g_bad_lists,
            ge_draw_tex_bind(), ge_draw_tex_fail());
    }
    if (fb) video_present(fb);  /* No-op on the GPU path. */
}

uint32_t ps2_ge_frames_drawn(void) { return g_frames; }
uint32_t ps2_ge_list_count(void) { return g_lists; }
uint32_t ps2_ge_prim_count(void) { return ge_draw_prims(); }
uint32_t ps2_ge_vtype_count(void) { return ge_draw_vtypes(); }
uint32_t ps2_ge_vtype_nonzero(void) { return ge_draw_vtypes_nonzero(); }
uint32_t ps2_ge_fb_addr(void) { return g_fb; }
uint32_t ps2_ge_fps_x100(void) {
    if (g_frames < 2 || g_tick1 <= g_tick0) return 0;
    return (uint32_t)((uint64_t)(g_frames - 1u) * 26812348000ull / (g_tick1 - g_tick0));
}
void ps2_ge_dump_request(void) { ge_draw_print_hist(); }
void ps2_ge_vram_copy(uint32_t dst, uint32_t src, uint32_t n) {
    /* dst is an offset into the 2 MB guest EDRAM, not a RAM address. */
    if (dst >= PSP_VRAM_SIZE || n > PSP_VRAM_SIZE - dst) return;
    uint8_t *pd = psp_vram_ptr() + dst;
    const uint8_t *ps = (const uint8_t *)psp_mem_ptr(src, n);
    if (ps) memmove(pd, ps, n);
}
