#ifndef DJMX_PS2_BGA_CLOCK_H
#define DJMX_PS2_BGA_CLOCK_H
#include <stdint.h>

/* Map the output mix timeline to music samples. A pause leaves a hole in the
 * mix timeline; a seek begins a new generation. Neither advances the movie. */
#define BGA_CLOCK_BLOCKS 128u
typedef struct { uint64_t at, music; uint32_t count; } ps2_bga_block;
typedef struct {
    ps2_bga_block blocks[BGA_CLOCK_BLOCKS];
    uint64_t submitted, first;
    uint32_t head, count;
} ps2_bga_clock;
static inline void ps2_bga_clock_seek(ps2_bga_clock *c, uint32_t sample) {
    c->submitted = c->first = sample;
    c->head = c->count = 0;
}
static inline void ps2_bga_clock_submit(ps2_bga_clock *c, uint64_t at, uint32_t n) {
    if (!n) return;
    ps2_bga_block *b = &c->blocks[c->head];
    b->at = at; b->music = c->submitted; b->count = n;
    c->submitted += n;
    c->head = (c->head + 1u) % BGA_CLOCK_BLOCKS;
    if (c->count < BGA_CLOCK_BLOCKS) c->count++;
    else c->first = c->blocks[c->head].music;
}
static inline uint64_t ps2_bga_clock_position(const ps2_bga_clock *c, uint64_t played) {
    uint64_t music = c->first;
    uint32_t i = (c->head + BGA_CLOCK_BLOCKS - c->count) % BGA_CLOCK_BLOCKS;
    for (uint32_t k = 0; k < c->count; k++, i = (i + 1u) % BGA_CLOCK_BLOCKS) {
        const ps2_bga_block *b = &c->blocks[i];
        if (played < b->at) return music;
        if (played < b->at + b->count) return b->music + played - b->at;
        music = b->music + b->count;
    }
    return music;
}
#endif
