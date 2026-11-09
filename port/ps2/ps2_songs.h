/* ps2_songs.h -- what tools-src/patch_songs.py writes into the emitted code (ps2_songs.c). */
#ifndef PS2_SONGS_H
#define PS2_SONGS_H
#include <stdint.h>

/* The song table (discstock.csv, 64 B per ID) moved from 0x4FCA48 (61 rows) to guest RAM
 * below the partition heap, with room for every u8 ID; its "last ID" word follows it. */
#define PS2_SONGTAB        0x08000000u
#define PS2_SONGTAB_ROWS   255u
#define PS2_SONGTAB_CNT    (PS2_SONGTAB_ROWS * 64u)          /* was +3904 */
#define PS2_SONGTAB_BYTES  (PS2_SONGTAB_CNT + 4u)            /* was 0xF44 */

/* Highest song ID the list builders walk (they test 0..LAST; unused rows are skipped). */
extern uint32_t ps2_song_last;
#define PS2_SONG_LAST      ps2_song_last

/* Selector list: entries (16 B: ID + 3 words) live outside the object, which keeps the
 * pointer in what was entry 0. */
#define PS2_LIST_CAP       256u
uint32_t ps2_list_alloc(uint32_t obj);
void     ps2_list_free(uint32_t obj);
uint32_t ps2_list_ent(uint32_t obj);
void ps2_list_trim(uint32_t obj, uint32_t protected_entry);

/* Per-song records (save struct +392 + 104*ID): IDs above 60 live in their own array.
 * off = 104*ID (+ a field offset below 104); returns what save + off would have been. */
uint32_t ps2_rec(uint32_t save, uint32_t off);

/* discstock.csv was just loaded into the table (the loader's last store). */
void ps2_songs_loaded(uint32_t table);
void ps2_songs_init(void);

/* Records of the extra songs, for ps2_savedata.c (a second file on the card). */
#define PS2_EXTREC_FIRST   61u
uint8_t *ps2_songs_extrec(uint32_t *size);
void     ps2_songs_extrec_reset(void);

/* Title atlas cell of a song ID: 4 sheets of 64 rows (256x1024, tools-src/titles.py), where
 * the game had 4 x 16. Anything out of range (an ID of -1, say) gets cell 255, blank.
 * Row heights in the drawing code are scaled by PS2_TROWH (16 rows of 1/16 -> 1/64). */
#define PS2_TSHEET(id)  ((uint32_t)(id) < 256u ? (uint32_t)(id) >> 6 : 3u)
#define PS2_TROWID(id)  ((uint32_t)(id) < 256u ? (uint32_t)(id) : 255u)
#define PS2_TROWH       0.25f

#endif
