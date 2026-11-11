/* ps2_atrac.h -- pure sceAtrac stream bookkeeping: no guest memory, no clock.
 *
 * Header-only so ps2_audio.c and the host test (test_atrac.c) share one copy.
 * Nothing is decoded: this tracks where the stream is, which is all the game's
 * BGM thread (0x7F064) can see. Model taken from observed PSP behaviour:
 *
 *   - RIFF "fact" holds {samples, delay}. The guest receives exactly `samples`
 *     samples. GetSoundSample reports samples-1 (the game adds 1 at 0x7ED94).
 *   - The decoder sits delay+69 (AT3) or delay+368 (AT3+) samples ahead of the
 *     guest position, so the first call is short: spf - (ahead % spf).
 *   - File frame 0 is consumed by SetData: a streamed track starts at data+bpf.
 *   - The ring end is frame-aligned after the header, and the header only
 *     counts until the read position first wraps.
 *
 * ponytail: `pos`, `valid` and the write offsets are byte accounting only. The
 * first 0x10000 bytes spill past the aligned ring end, so reading frames straight
 * from buf+pos would be wrong near a wrap: the real-audio milestone needs a FIFO
 * in the order bytes were added. Same for the frames left in the ring at a loop.
 */
#ifndef DJMX_PS2_ATRAC_H
#define DJMX_PS2_ATRAC_H

#include <stdint.h>

#define ATRAC_ERR_BAD_ID      0x80630005u
#define ATRAC_ERR_FORMAT      0x80630006u
#define ATRAC_ERR_ALL_LOADED  0x80630009u
#define ATRAC_ERR_NO_DATA     0x80630010u
#define ATRAC_ERR_TOO_BIG     0x80630018u
#define ATRAC_ERR_FIRST_RESET 0x80630016u
#define ATRAC_ERR_EMPTY       0x80630023u   /* game keeps feeding (0x7F0BC) */
#define ATRAC_ERR_AFTER_END   0x80630024u   /* game raises its end flag (0x7F0C8) */

#define ATRAC_ALL_ON_MEMORY      (-1)
#define ATRAC_NOLOOP_ON_MEMORY   (-2)
#define ATRAC_LOOP_ON_MEMORY     (-3)

enum { ATRAC_ALL_LOADED = 2, ATRAC_STREAM_NOLOOP = 4, ATRAC_STREAM_LOOP_END = 5, ATRAC_STREAM_LOOP_TRAILER = 6 };

typedef struct {
    int      used, state, channels;
    int32_t  spf;              /* samples per frame */
    int32_t  delay;            /* fact delay */
    int32_t  ahead;            /* delay + codec extra */
    int32_t  end;              /* last valid guest sample */
    int32_t  loop_start, loop_end;   /* guest samples; has_loop says if they mean anything */
    int32_t  loop_end_raw;
    int      has_loop;
    int32_t  cur;              /* next guest sample */
    int32_t  loop_num;         /* -1 forever */
    uint32_t buf, buf_size;    /* guest ring */
    uint32_t file_size, data_off, bpf;
    uint32_t file_off;         /* file offset of the next byte to feed */
    uint32_t pos;              /* ring offset of the next frame */
    uint32_t valid;            /* bytes fed and not yet consumed */
    uint32_t hdr;              /* RIFF prefix still in the ring, 0 after the first wrap */
    uint32_t consumed;         /* frames taken from the ring so far (the decoder follows it) */
} atrac_t;

static inline uint32_t atrac_rd32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline uint32_t atrac_rd16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
#define ATRAC_FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

static inline int atrac_streamed(const atrac_t *a) { return a->state != ATRAC_ALL_LOADED; }

static inline uint32_t atrac_file_offset(const atrac_t *a, int32_t sample) {
    int32_t frame = (sample + a->delay) / a->spf;
    if (frame < -1) frame = -1;              /* never before the data chunk */
    return (uint32_t)((int32_t)(a->data_off + a->bpf) + frame * (int32_t)a->bpf);
}

/* Parse the first `n` bytes of a track held in a guest buffer of the same size.
 * Returns 0 or an ATRAC error; `a` is only valid on 0. */
static inline uint32_t atrac_set_data(atrac_t *a, uint32_t buf, const uint8_t *p, uint32_t n) {
    uint32_t tag = 0, data_size = 0, fact = 0;
    int have_fact = 0;

    a->used = 0;
    a->delay = 0;
    a->has_loop = 0;
    a->loop_start = a->loop_end = a->loop_end_raw = -1;
    a->bpf = a->data_off = 0;
    a->channels = 0;
    if (n < 12 || atrac_rd32(p) != ATRAC_FOURCC('R', 'I', 'F', 'F') ||
        atrac_rd32(p + 8) != ATRAC_FOURCC('W', 'A', 'V', 'E'))
        return ATRAC_ERR_FORMAT;
    a->file_size = atrac_rd32(p + 4) + 8;

    int32_t loop_raw = -1;
    for (uint32_t off = 12; off + 8 <= n && !a->data_off; ) {
        const uint32_t id = atrac_rd32(p + off), sz = atrac_rd32(p + off + 4), body = off + 8;
        if (id == ATRAC_FOURCC('f', 'm', 't', ' ') && sz >= 16 && body + 16 <= n) {
            tag = atrac_rd16(p + body);
            a->channels = (int)atrac_rd16(p + body + 2);
            a->bpf = atrac_rd16(p + body + 12);
        } else if (id == ATRAC_FOURCC('f', 'a', 'c', 't') && sz >= 4 && body + sz <= n) {
            fact = atrac_rd32(p + body);
            have_fact = 1;
            if (sz >= 8) a->delay = (int32_t)atrac_rd32(p + body + 4);
        } else if (id == ATRAC_FOURCC('s', 'm', 'p', 'l') && sz >= 60 && body + 60 <= n) {
            if (atrac_rd32(p + body + 28) >= 1) {       /* first loop record at +36 */
                loop_raw = (int32_t)atrac_rd32(p + body + 44);
                a->loop_end_raw = (int32_t)atrac_rd32(p + body + 48);
                a->has_loop = 1;
            }
        } else if (id == ATRAC_FOURCC('d', 'a', 't', 'a')) {
            a->data_off = body;
            data_size = sz;
        }
        if (sz > 0x7FFFFFFFu) return ATRAC_ERR_FORMAT;
        off = body + sz + (sz & 1u);
    }

    if (tag == 0x0270u)      { a->spf = 1024; a->ahead = a->delay + 69; }
    else if (tag == 0xFFFEu) { a->spf = 2048; a->ahead = a->delay + 368; }
    else return ATRAC_ERR_FORMAT;
    if (!a->bpf || !a->data_off || a->channels < 1 || a->channels > 2) return ATRAC_ERR_FORMAT;

    /* ponytail: no fact chunk means no delay; every file of this game has one (554 of 554). */
    a->end = (int32_t)(have_fact ? fact : data_size / a->bpf * (uint32_t)a->spf) - 1;
    if (a->has_loop) {
        a->loop_start = loop_raw - a->ahead;
        a->loop_end = a->loop_end_raw - a->ahead;
    }

    a->cur = 0;
    a->loop_num = 0;
    a->buf = buf;
    a->buf_size = n;
    a->file_off = n < a->file_size ? n : a->file_size;
    if (n >= a->file_size) {
        a->state = ATRAC_ALL_LOADED;
        a->pos = a->valid = a->hdr = 0;
    } else {
        if (!a->has_loop) a->state = ATRAC_STREAM_NOLOOP;
        else if (a->loop_end_raw == a->end + a->ahead) a->state = ATRAC_STREAM_LOOP_END;
        else a->state = ATRAC_STREAM_LOOP_TRAILER;
        a->hdr = a->data_off;
        a->pos = a->data_off + a->bpf;
        a->valid = n > a->pos ? n - a->pos : 0;
    }
    a->used = 1;
    return 0;
}

static inline uint32_t atrac_ring_end(const atrac_t *a) {
    return (a->buf_size - a->hdr) / a->bpf * a->bpf + a->hdr;
}

static inline void atrac_consume(atrac_t *a) {
    a->consumed++;
    a->pos += a->bpf;
    a->valid = a->valid > a->bpf ? a->valid - a->bpf : 0;
    if (a->pos >= atrac_ring_end(a)) {
        a->pos -= atrac_ring_end(a);
        a->hdr = 0;
    }
}

static inline int32_t atrac_remain(const atrac_t *a) {
    if (a->state == ATRAC_ALL_LOADED) return ATRAC_ALL_ON_MEMORY;
    if (a->file_off >= a->file_size) {
        if (a->state == ATRAC_STREAM_NOLOOP) return ATRAC_NOLOOP_ON_MEMORY;
        if (a->state == ATRAC_STREAM_LOOP_TRAILER && a->cur > a->loop_end) return ATRAC_NOLOOP_ON_MEMORY;
        if (a->loop_num == 0) return ATRAC_LOOP_ON_MEMORY;
    }
    return (int32_t)(a->valid / a->bpf);
}

/* Where the guest may write next (ring offset), how much, and from which file offset. */
static inline void atrac_stream_info(const atrac_t *a, uint32_t *offset, uint32_t *writable, uint32_t *read_off) {
    uint32_t rd = a->file_off, o = 0, w = 0;
    if (a->state == ATRAC_ALL_LOADED) {
        rd = 0;
    } else {
        const uint32_t end = atrac_ring_end(a), ext = a->pos + a->valid;
        if (ext < end) {
            o = ext;
            w = end - ext;
        } else {
            o = ext - end;
            w = a->pos > o ? a->pos - o : 0;
        }
        if (rd >= a->file_size) {
            if (a->state == ATRAC_STREAM_NOLOOP) { rd = 0; o = 0; w = 0; }
            else rd = atrac_file_offset(a, a->loop_start - a->spf * 2);
        }
        if (rd + w > a->file_size) w = a->file_size - rd;
        if (o + w > a->buf_size) { o = 0; w = a->buf_size; }  /* cannot happen; never write outside the ring */
    }
    *offset = o;
    *writable = w;
    *read_off = rd;
}

static inline uint32_t atrac_add(atrac_t *a, uint32_t n) {
    uint32_t o, w, rd;
    if (a->state == ATRAC_ALL_LOADED) return ATRAC_ERR_ALL_LOADED;
    atrac_stream_info(a, &o, &w, &rd);
    if (n > w) return ATRAC_ERR_TOO_BIG;
    if (n) a->file_off = rd + n;
    a->valid += n;
    return 0;
}

/* One sceAtracDecodeData. Fills *samples and *finish; the caller clears the PCM. */
static inline uint32_t atrac_decode(atrac_t *a, uint32_t *samples, uint32_t *finish) {
    uint32_t got = 0;
    *samples = 0;
    *finish = 0;
    if (a->cur > a->end && a->loop_num == 0) {
        *finish = 1;
        return ATRAC_ERR_AFTER_END;
    }

    const int32_t skip = (a->ahead + a->cur) % a->spf;
    uint32_t max = (uint32_t)(a->end + 1 - a->cur);
    if (skip) max = (uint32_t)(a->spf - skip);

    /* After a loop the frame that only loads decoder state goes first. */
    const int prime = skip && a->hdr == 0 && atrac_streamed(a);
    int have;
    if (atrac_streamed(a))
        have = a->valid >= a->bpf * (prime ? 2u : 1u);
    else
        have = atrac_file_offset(a, a->cur - skip) < a->file_off;

    if (!have && atrac_streamed(a) && a->file_off < a->file_size)
        return ATRAC_ERR_EMPTY;              /* nothing consumed: AddStreamData resumes */

    if (have) {
        if (prime) atrac_consume(a);
        got = (uint32_t)(a->spf - skip);
        if (got > max) got = max;
        if (atrac_streamed(a)) atrac_consume(a);
        a->cur += (int32_t)got;
    }
    *samples = got;

    const int hit_end = a->cur > a->end || !have;
    if (a->has_loop && a->loop_num != 0 && (hit_end || a->cur > a->loop_end)) {
        a->cur = a->loop_start;
        if (a->loop_num > 0) a->loop_num--;
        if (atrac_streamed(a)) {
            const uint32_t lo = atrac_file_offset(a, a->loop_start - a->spf * 2);
            if (lo > a->file_off || lo + a->valid < a->file_off) a->file_off = lo;
        }
    } else if (hit_end) {
        *finish = 1;
        a->cur += a->spf - (int32_t)got;     /* so a track that ran dry still reaches AFTER_END */
    }
    return 0;
}

/* sceAtracGetBufferInfoForResetting: first span only, the reset never needs a second. */
static inline void atrac_reset_info(const atrac_t *a, int32_t sample, uint32_t *writable, uint32_t *min_write, uint32_t *file_pos) {
    if (a->state == ATRAC_ALL_LOADED) {
        *writable = *min_write = *file_pos = 0;
        return;
    }
    uint32_t off = atrac_file_offset(a, sample - a->delay - a->spf);
    const uint32_t ring = a->buf_size / a->bpf * a->bpf;
    const uint32_t left = a->file_size > off ? a->file_size - off : 0;
    *writable = left < ring ? left : ring;
    *min_write = a->bpf * ((sample + a->delay) % a->spf >= a->spf - (a->ahead - a->delay) ? 3u : 2u);
    if (sample < a->delay && off != a->data_off) off -= a->bpf;
    *file_pos = off;
}

/* sceAtracResetPlayPosition after `written` bytes were put at the start of the ring. */
static inline uint32_t atrac_reset(atrac_t *a, int32_t sample, uint32_t written) {
    uint32_t w, m, f;
    atrac_reset_info(a, sample, &w, &m, &f);
    if (written < m || written > w) return ATRAC_ERR_FIRST_RESET;
    if (atrac_streamed(a)) {
        a->file_off = f + written;
        a->hdr = 0;
        a->pos = a->bpf;
        a->valid = written > a->bpf ? written - a->bpf : 0;
    }
    a->cur = sample;
    return 0;
}

#endif /* DJMX_PS2_ATRAC_H */
