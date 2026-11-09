/* test_atrac.c -- host check of the sceAtrac stream bookkeeping (ps2_atrac.h).
 *
 *   gcc -O1 -Wall -I. test_atrac.c -o /tmp/test_atrac && /tmp/test_atrac file.at3 ...
 *
 * Plays each real .at3 with the logic of the game's BGM thread (0x7EC48 + 0x7F064):
 * set the first 64 KB, refill when the remaining frames drop to a quarter of the ring
 * (remain/2 >> 1), read through a file position that seeks only when it differs. Asserts
 * the track ends with 0x80630024 at the exact sample count of the RIFF fact chunk, that
 * every file byte was fed once and in order, and that no write leaves the ring.
 */
#include "ps2_atrac.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RING 0x10000u
#define BUF  0x000E8100u          /* the game's ring address, only used as a number */

typedef struct {
    uint64_t total;               /* samples handed to the guest */
    uint32_t decodes, reads, seeks, empties;
    uint32_t fed_contiguous;      /* bytes fed in file order from the first read */
    uint32_t first_call;
    int      ended;
} result;

/* io_lag: passes a read stays in flight, to reach the buffer-empty path. */
static result play(const uint8_t *file, uint32_t size, int32_t loop_num, uint64_t stop_after, int io_lag) {
    atrac_t a;
    result r = {0};
    assert(atrac_set_data(&a, BUF, file, RING) == 0);
    assert(atrac_streamed(&a));
    a.loop_num = loop_num;
    r.fed_contiguous = RING;

    int32_t remain = atrac_remain(&a);
    int32_t threshold = (remain + (int32_t)((uint32_t)(remain >> 1) >> 31)) >> 1;
    if (threshold < 1) threshold = 1;

    uint32_t file_pos = RING;     /* ctx+24 */
    int pending = 0, lag = 0;     /* ctx+40 */
    uint32_t got = 0;

    for (;;) {
        uint32_t n, fin;
        const uint32_t e = atrac_decode(&a, &n, &fin);
        if (e == ATRAC_ERR_AFTER_END) { assert(n == 0); r.ended = 1; break; }
        assert(e == 0 || e == ATRAC_ERR_EMPTY);
        if (e == ATRAC_ERR_EMPTY) { assert(n == 0); r.empties++; }
        remain = e == ATRAC_ERR_EMPTY ? 0 : atrac_remain(&a);
        if (r.decodes == 0) r.first_call = n;
        r.decodes++;
        r.total += n;
        if (stop_after && r.total >= stop_after) break;

        if (pending == 0 && !(threshold < remain) && remain != -1 && remain != -2) {
            uint32_t off, w, rd;
            atrac_stream_info(&a, &off, &w, &rd);
            if (w > 0x40000u) w = 0x40000u;
            assert(off + w <= RING);
            if (file_pos != rd) {
                /* Only the loop re-read after a whole lap moves the file position. */
                if (!r.seeks) assert(r.fed_contiguous == size);
                file_pos = rd;
                r.seeks++;
            }
            got = rd + w <= size ? w : (rd < size ? size - rd : 0);
            pending = 1;
            lag = io_lag;
            r.reads++;
            /* Only the first lap is a straight file read. */
            if (!r.seeks) { assert(rd == r.fed_contiguous); r.fed_contiguous += got; }
        }
        if (pending > 0 && (remain == 0 || lag-- <= 0)) {
            assert(atrac_add(&a, got) == 0);
            file_pos += got;
            pending = 0;
        }
        assert(a.valid <= RING);
        assert(a.pos < RING);
        assert(r.decodes < 100000000u);
    }
    return r;
}

static void test_file(const char *path) {
    FILE *f = fopen(path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    const uint32_t size = (uint32_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc(size);
    assert(file && fread(file, 1, size, f) == size);
    fclose(f);

    atrac_t a;
    assert(atrac_set_data(&a, BUF, file, RING) == 0);
    assert(a.file_size == size);
    const uint64_t fact = (uint64_t)a.end + 1;
    const uint32_t expect_first = (uint32_t)(a.spf - (a.ahead % a.spf));

    /* 1. A song: loop count 0, played once. */
    result r = play(file, size, 0, 0, 0);
    printf("%s\n  fact=%llu bpf=%u spf=%d delay=%d state=%d: total=%llu decodes=%u first=%u reads=%u seeks=%u empties=%u\n",
           path, (unsigned long long)fact, a.bpf, a.spf, a.delay, a.state,
           (unsigned long long)r.total, r.decodes, r.first_call, r.reads, r.seeks, r.empties);
    assert(r.ended);
    assert(r.total == fact);
    assert(r.first_call == expect_first);
    assert(r.empties == 0);
    assert(r.fed_contiguous == size);    /* whole file, once, in order, then only loop re-reads */

    /* 2. Same song with slow reads: it may run dry, and must lose nothing when it does. */
    r = play(file, size, 0, 0, 400);
    printf("  slow io: total=%llu empties=%u\n", (unsigned long long)r.total, r.empties);
    assert(r.ended && r.total == fact);

    /* 3. Direct starvation: no AddStreamData at all, then the error, then resume. */
    atrac_t s;
    assert(atrac_set_data(&s, BUF, file, RING) == 0);
    uint32_t n, fin, last = 0, before;
    uint64_t total = 0;
    while ((last = atrac_decode(&s, &n, &fin)) == 0) total += n;
    assert(last == ATRAC_ERR_EMPTY && n == 0 && fin == 0);
    before = s.valid;
    assert(atrac_decode(&s, &n, &fin) == ATRAC_ERR_EMPTY);     /* repeating consumes nothing */
    assert(s.valid == before);
    uint32_t off, w, rd;
    atrac_stream_info(&s, &off, &w, &rd);
    assert(w > 0 && rd == RING);
    assert(atrac_add(&s, w + 1) == ATRAC_ERR_TOO_BIG);
    assert(atrac_add(&s, w) == 0);
    assert(atrac_decode(&s, &n, &fin) == 0 && n == (uint32_t)s.spf);

    /* 4. Looping: two repeats, then the end. A repeat restarts at loop_start. */
    const uint64_t lap = fact - (uint64_t)(int64_t)a.loop_start;
    r = play(file, size, 2, 0, 0);
    printf("  loop x2: total=%llu reads=%u seeks=%u (lap %llu)\n",
           (unsigned long long)r.total, r.reads, r.seeks, (unsigned long long)lap);
    assert(r.ended);
    assert(r.total == fact + 2 * lap);

    /* 5. Forever (the menus): keeps delivering, the feed never wedges. */
    r = play(file, size, -1, fact * 3, 0);
    assert(!r.ended && r.total >= fact * 3);

    /* 6. Reset as the game drives it at 0x7F628: no data read, writable bytes claimed. */
    assert(atrac_set_data(&s, BUF, file, RING) == 0);
    uint32_t m, fp;
    atrac_reset_info(&s, 0, &w, &m, &fp);
    assert(w >= m && w <= RING && fp >= s.data_off);
    assert(atrac_reset(&s, 0, w) == 0);
    assert(s.cur == 0 && s.file_off == fp + w);
    assert(atrac_reset(&s, 0, 0) == ATRAC_ERR_FIRST_RESET);

    /* 7. A track smaller than the read: all on memory, no feeding, exact end. */
    uint8_t *small = malloc(RING * 2);
    const uint32_t frames = 40;
    memcpy(small, file, a.data_off);
    const uint32_t small_size = a.data_off + frames * a.bpf;
    small[4] = (uint8_t)(small_size - 8); small[5] = (uint8_t)((small_size - 8) >> 8);
    small[6] = (uint8_t)((small_size - 8) >> 16); small[7] = (uint8_t)((small_size - 8) >> 24);
    /* fact: frames minus the skipped lead-in */
    const uint32_t small_fact = frames * (uint32_t)a.spf - (uint32_t)a.ahead;
    for (uint32_t o = 12; o + 8 <= a.data_off; ) {
        const uint32_t sz = atrac_rd32(small + o + 4);
        if (atrac_rd32(small + o) == ATRAC_FOURCC('f', 'a', 'c', 't')) {
            small[o + 8] = (uint8_t)small_fact; small[o + 9] = (uint8_t)(small_fact >> 8);
            small[o + 10] = (uint8_t)(small_fact >> 16); small[o + 11] = (uint8_t)(small_fact >> 24);
        }
        o += 8 + sz + (sz & 1u);
    }
    assert(atrac_set_data(&s, BUF, small, RING) == 0);
    assert(s.state == ATRAC_ALL_LOADED);
    assert(atrac_remain(&s) == ATRAC_ALL_ON_MEMORY);
    total = 0;
    while ((last = atrac_decode(&s, &n, &fin)) == 0) total += n;
    printf("  all on memory: total=%llu of %u\n", (unsigned long long)total, small_fact);
    assert(last == ATRAC_ERR_AFTER_END && total == small_fact);
    assert(atrac_add(&s, 1) == ATRAC_ERR_ALL_LOADED);
    free(small);

    free(file);
}

int main(int argc, char **argv) {
    assert(argc > 1);
    for (int i = 1; i < argc; i++) test_file(argv[i]);
    printf("OK: %d file(s)\n", argc - 1);
    return 0;
}
