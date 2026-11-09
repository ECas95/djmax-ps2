/* ps2_songs.c -- more than 60 songs (analysis/25-mas-de-60-canciones.md).
 *
 * The game keeps its 60 songs in three fixed places. tools-src/patch_songs.py rewrites the
 * emitted code that addresses them so that they can grow:
 * - the song table (0x4FCA48, 61 rows) -> PS2_SONGTAB, 255 rows;
 * - the selector list object (61 entries inside it, on the stack of 30 screens) -> its
 *   entries in a block of their own, pointed to from the object;
 * - the records in the save struct (+392 + 104*ID, IDs up to 60) -> IDs above 60 in an
 *   array of their own, kept in a second file on the card by ps2_savedata.c.
 * All of it lives in guest RAM 0x08000000-0x0801FFFF, below the partition heap
 * (ps2_hle.c HEAP_LO), which nothing else uses.
 *
 * The songs themselves come from tools-src/songpack.py: their files are in the overlays of
 * all.mp / all2.mp (ps2_hle.c), their rows (IDs 61..) in host:PS2_DATA/ovl/songs.bin.
 * Without that file the game has its own 60 songs and nothing else changes.
 */
#include "runtime_min.h"
#include "ps2_songs.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#define EXTREC_BASE   0x08004000u
#define EXTREC_N      (PS2_SONGTAB_ROWS - PS2_EXTREC_FIRST)            /* 194 */
#define EXTREC_BYTES  (EXTREC_N * 104u)
#define LIST_BASE     0x0800A000u
#define LIST_BLOCK    (PS2_LIST_CAP * 16u)                             /* 4 KB */
#define LIST_N        22u                                              /* up to 0x08020000 */

uint32_t ps2_song_last = 60u;
static uint8_t  g_rows[(PS2_SONGTAB_ROWS - PS2_EXTREC_FIRST) * 64u];
static uint32_t g_nrows;

/* ---- list entries ---- */
static uint32_t g_list_owner[LIST_N];

uint32_t ps2_list_alloc(uint32_t obj) {
    for (uint32_t i = 0; i < LIST_N; i++)
        if (g_list_owner[i] == obj) g_list_owner[i] = 0;      /* re-constructed in place */
    for (uint32_t i = 0; i < LIST_N; i++)
        if (!g_list_owner[i]) {
            const uint32_t blk = LIST_BASE + i * LIST_BLOCK;
            g_list_owner[i] = obj;
            psp_write32(obj, blk);
            return blk;
        }
    psp_printf("[songs] FATAL: more than %u live song lists\n", LIST_N);
    for (;;) {}
}

void ps2_list_free(uint32_t obj) {
    for (uint32_t i = 0; i < LIST_N; i++)
        if (g_list_owner[i] == obj) g_list_owner[i] = 0;
}

uint32_t ps2_list_ent(uint32_t obj) { return psp_read32(obj); }

/* The PSP catalog cached every visited jacket until leaving the selector. With
 * 116 entries, GIM objects (~54 KB each) plus loose pixels fill the 15.75 MB VPL.
 * Keep the current/animated neighbours, invalidate older slots and release the
 * owning reference through the game's own manager. A revisit loads it normally.
 */
void ps2_list_trim(uint32_t obj, uint32_t protected_entry) {
    int owned = 0;
    for (uint32_t i=0; i<LIST_N; i++) if (g_list_owner[i] == obj) owned = 1;
    if (!owned) return;
    const uint32_t base = ps2_list_ent(obj), count = psp_read32(obj + 980u);
    const uint32_t selected = psp_read32(obj + 988u);
    if (!count || count > PS2_LIST_CAP || selected >= count) return;
    uint32_t released = 0;
    for (uint32_t i=0; i<count; i++) {
        uint32_t distance = i > selected ? i-selected : selected-i;
        if (count-distance < distance) distance = count-distance;
        const uint32_t entry = base + i*16u;
        if (distance <= 4u || entry == protected_entry) continue;
        for (uint32_t variant=0; variant<3u; variant++) {
            const uint32_t slot = entry + 4u + variant*4u;
            const uint32_t handle = psp_read32(slot);
            if (handle == ~0u) continue;
            psp_write32(slot, ~0u);
            psp_cpu_state saved = psp_cpu;
            psp_cpu.r[PSP_REG_A0] = handle;
            psp_cpu.r[PSP_REG_SP] -= 64u;
            psp_call_from_host(0x00006634u);
            psp_cpu = saved;
            released++;
        }
    }
    if (released) { static uint32_t logged;
        if (logged++ < 12u) psp_printf("[songs] jacket cache: released %u, selected %u/%u\n", released, selected, count);
    }
}

/* ---- records ---- */
uint32_t ps2_rec(uint32_t save, uint32_t off) {
    const uint32_t id = off / 104u;
    if (id < PS2_EXTREC_FIRST) return save + off;
    if (id >= PS2_SONGTAB_ROWS) { psp_printf("[songs] record of ID %u?\n", id); return save + off; }
    return EXTREC_BASE + (id - PS2_EXTREC_FIRST) * 104u + off % 104u - 392u;
}

uint8_t *ps2_songs_extrec(uint32_t *size) {
    *size = EXTREC_BYTES;
    return (uint8_t *)psp_mem_ptr(EXTREC_BASE, EXTREC_BYTES);
}

/* A new record, as the game makes one (0x8FED4, 0x904CC): zero, ranks 0x16, and the song
 * and its modes open (2) like an original that is open at the start. */
void ps2_songs_extrec_reset(void) {
    uint8_t *r = (uint8_t *)psp_mem_ptr(EXTREC_BASE, EXTREC_BYTES);
    if (!r) return;
    memset(r, 0, EXTREC_BYTES);
    for (uint32_t i = 0; i < EXTREC_N; i++) {
        uint8_t *e = r + i * 104u;
        for (uint32_t k = 0; k < 5u; k++) e[k] = 2;
        memset(e + 56, 0x16, 48);              /* per mode and difficulty, 4 bytes each */
    }
}

/* ---- the table ---- */
void ps2_songs_init(void) {
    ps2_songs_extrec_reset();                     /* before the game; a load overwrites */
#ifdef DJMAX_BASE_ONLY
    return;                                     /* original catalog; no external songs */
#endif
    const int fd = open("host:PS2_DATA/ovl/songs.bin", O_RDONLY);
    if (fd < 0) { psp_printf("[songs] no songs.bin: the game's own 60\n"); return; }
    uint32_t h[2] = {0, 0};
    if (read(fd, h, 8) == 8 && h[0] == 0x31474E53u /* "SNG1" */ && h[1] <= sizeof g_rows / 64u &&
        read(fd, g_rows, h[1] * 64u) == (int)(h[1] * 64u)) {
        g_nrows = h[1];
        ps2_song_last = PS2_EXTREC_FIRST + g_nrows - 1u;
    }
    close(fd);
    psp_printf("[songs] %u extra songs, IDs %u-%u\n", g_nrows, PS2_EXTREC_FIRST, ps2_song_last);
}

void ps2_songs_loaded(uint32_t table) {
    uint8_t *t = (uint8_t *)psp_mem_ptr(table, PS2_SONGTAB_BYTES);
    if (!t) return;
    const uint32_t rows = psp_read32(table + PS2_SONGTAB_CNT);
    memset(t + 60u * 64u, 0, (PS2_SONGTAB_ROWS - 60u) * 64u);          /* 60: RANDOM, no row */
    if (g_nrows) {
        memcpy(t + PS2_EXTREC_FIRST * 64u, g_rows, g_nrows * 64u);
        psp_write32(table + PS2_SONGTAB_CNT, ps2_song_last);
    }
    static int logged;
    if (!logged++) psp_printf("[songs] table at 0x%08X: the game read %u, last ID %u\n", table, rows, ps2_song_last);
}
