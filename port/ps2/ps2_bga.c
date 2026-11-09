/* Stream independent compressed INDEX8 frames into the game's marked BGA texture.
 * The music output sample counter is the clock: pause/restart follow audio.
 * Workspace comes from guest sysmem, not the almost-full EE image/heap.
 */
#include "ps2_bga.h"
#include "ps2_hle.h"
#include "runtime_min.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#define WORK_BYTES  (224u * 1024u)
#define FRAME_BYTES (1024u + 65536u)
#define PACK_BYTES  (68u * 1024u)
static uint32_t g_uid, g_last = ~0u, g_header[8], g_idle, g_seen;
static uint64_t g_samples;
static uint8_t *g_work;
static uint32_t g_zused;
static int g_fd = -1;
static char g_name[17];

void ps2_bga_audio_reset(void) { g_samples = 0; g_last = ~0u; }
void ps2_bga_audio_seek(uint32_t sample) { g_samples = sample; g_last = ~0u; }
void ps2_bga_audio_samples(uint32_t n) { g_samples += n; }
static voidpf zalloc_bga(voidpf opaque, uInt count, uInt size) {
    (void)opaque;
    uint32_t n = count * size;
    uint32_t off = (g_zused + 15u) & ~15u;
    if (n > WORK_BYTES - PACK_BYTES - FRAME_BYTES || off > WORK_BYTES - PACK_BYTES - FRAME_BYTES - n) return NULL;
    g_zused = off + n;
    void *p = g_work + PACK_BYTES + FRAME_BYTES + off;
    memset(p, 0, n);
    return p;
}
static void zfree_bga(voidpf opaque, voidpf address) { (void)opaque; (void)address; }

static int open_bga(const char *name) {
    if (g_fd >= 0) close(g_fd);
    g_fd = -1;
    strncpy(g_name, name, 16); g_name[16] = 0;
    char path[96];
    snprintf(path, sizeof path, "host:PS2_DATA/bga/%s.bgs", name);
    g_fd = open(path, O_RDONLY);
    if (g_fd < 0) { psp_printf("[bga] missing %s\n", path); return 0; }
    if (read(g_fd, g_header, 32) != 32 || g_header[0] != 0x31534742u ||
        g_header[1] != 256u || g_header[2] != 256u || !g_header[3] || !g_header[4] ||
        !g_header[5] || g_header[5] >= 65536u || g_header[3] > 120000u || g_header[4] > 10000u ||
        g_header[6] > PACK_BYTES || g_header[7] != 32u + (g_header[5] + 1u) * 4u) {
        close(g_fd); g_fd = -1; return 0;
    }
    if (!g_work) {
        g_uid = ps2_sysmem_alloc(WORK_BYTES, 0, 0);
        if (g_uid) g_work = psp_mem_ptr(ps2_sysmem_head(g_uid), WORK_BYTES);
    }
    if (!g_work) { close(g_fd); g_fd = -1; psp_printf("[bga] workspace allocation failed\n"); return 0; }
    g_last = ~0u;
    psp_printf("[bga] %s: %u frames, %u/%u fps, 224 KB workspace\n", name, g_header[5], g_header[3], g_header[4]);
    return 1;
}

uint32_t ps2_bga_texture(uint8_t *pixels, uint8_t *palette) {
    if (memcmp(palette + 960, "BGS1", 4)) return 0;
    char name[17]; memcpy(name, palette + 964, 16); name[16] = 0;
    for (unsigned i=0; name[i]; i++)
        if (!((name[i]>='a' && name[i]<='z') || (name[i]>='0' && name[i]<='9') || name[i]=='_')) return 0;
    g_seen = 1; g_idle = 0;
    if (strcmp(g_name, name)) open_bga(name);
    if (g_fd < 0) return 0;
    uint32_t frame = (uint32_t)(g_samples * g_header[3] / (44100ull * g_header[4]));
    if (frame >= g_header[5]) frame = g_header[5] - 1u;
    if (frame != g_last) {
        uint32_t offsets[2];
        if (lseek(g_fd, 32u + frame * 4u, SEEK_SET) < 0 || read(g_fd, offsets, 8) != 8 ||
            offsets[0] < g_header[7] || offsets[1] <= offsets[0] || offsets[1]-offsets[0] > PACK_BYTES) return 0;
        uint32_t size = offsets[1] - offsets[0];
        if (lseek(g_fd, offsets[0], SEEK_SET) < 0 || read(g_fd, g_work, size) != (int)size) return 0;
        z_stream z; memset(&z, 0, sizeof z); g_zused = 0;
        z.zalloc = zalloc_bga; z.zfree = zfree_bga;
        z.next_in = g_work; z.avail_in = size;
        z.next_out = g_work + PACK_BYTES; z.avail_out = FRAME_BYTES;
        if (inflateInit(&z) != Z_OK) return 0;
        int result = inflate(&z, Z_FINISH);
        int valid = result == Z_STREAM_END && z.total_out == FRAME_BYTES;
        inflateEnd(&z);
        if (!valid) { psp_printf("[bga] %s frame %u inflate=%d\n", name, frame, result); return 0; }
        uint32_t previous = g_last;
        memcpy(palette, g_work + PACK_BYTES, 960);
        memcpy(pixels, g_work + PACK_BYTES + 1024, 65536);
        g_last = frame;
        if (previous == ~0u || frame / 300u != previous / 300u)
            psp_printf("[bga] %s frame %u time=%u ms\n", name, frame, (uint32_t)(g_samples * 1000u / 44100u));
    }
    return frame + 1u; /* forces cache invalidation even if sampled pixels match */
}

void ps2_bga_frame(void) {
    if (g_seen) { g_seen = 0; return; }
    if (++g_idle == 60u) {
        if (g_fd >= 0) close(g_fd);
        g_fd = -1; g_name[0] = 0;
        if (g_uid) ps2_sysmem_free_block(g_uid);
        g_uid = 0; g_work = NULL;
    }
}
