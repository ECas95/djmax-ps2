/* psprecomp — IoFileMgrForUser.
 *
 * The PSP's file API, mapped onto a host directory. Games address the UMD as
 * `disc0:/` and the Memory Stick as `ms0:/`, so those prefixes are rewritten to
 * subdirectories of a root the host chooses.
 *
 * Reads go straight into guest memory, which means a game loading assets is
 * doing the real thing -- and a texture or model that arrives byte-correct is
 * strong evidence the recompiled code around it is behaving too.
 */

#include "psprecomp/hle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <io.h>
#  include <direct.h>   /* _mkdir / _rmdir */
#else
#  include <dirent.h>
#endif

#define MAX_FILES 64
#define MAX_DIRS  16

/* Open flags, as the guest passes them. */
#define PSP_O_RDONLY 0x0001
#define PSP_O_WRONLY 0x0002
#define PSP_O_RDWR   0x0003
#define PSP_O_APPEND 0x0100
#define PSP_O_CREAT  0x0200
#define PSP_O_TRUNC  0x0400

typedef struct { FILE *f; int used; } io_file;

typedef struct {
    int used;
#ifdef _WIN32
    intptr_t handle;
    struct _finddata_t data;
    int first;
    int done;
#else
    DIR *dir;
#endif
} io_dir;

static io_file g_file[MAX_FILES];
static io_dir  g_dir[MAX_DIRS];
static char    g_root[512];
static uint64_t g_bytes_read;

void psp_io_set_root(const char *root) {
    snprintf(g_root, sizeof g_root, "%s", root ? root : ".");
}

void psp_io_reset(void) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (g_file[i].used && g_file[i].f) fclose(g_file[i].f);
        g_file[i].used = 0;
        g_file[i].f = NULL;
    }
    memset(g_dir, 0, sizeof g_dir);
    g_bytes_read = 0;
    if (!g_root[0]) psp_io_set_root(".");
}

void psp_io_init(void) { g_root[0] = '\0'; psp_io_reset(); }

uint64_t psp_io_bytes_read(void) { return g_bytes_read; }

/* Rewrite a PSP path into a host path.
 *
 * `disc0:`/`umd0:` ARE the root: a disc dump is exactly the tree the console
 * sees, so `disc0:/PSP_GAME/USRDIR/all.mp` is `<root>/PSP_GAME/USRDIR/all.mp`.
 * (This used to insert a `disc/` subdirectory, which meant a host pointing the
 * root at an extracted ISO found nothing.) The other devices keep a
 * subdirectory so a memory-stick tree can sit beside the disc without colliding. */
static void map_path(const char *guest, char *out, size_t cap) {
    const char *p = guest;
    const char *sub = "";

    if      (!strncmp(p, "disc0:", 6)) { p += 6; sub = "";      }
    else if (!strncmp(p, "umd0:",  5)) { p += 5; sub = "";      }
    else if (!strncmp(p, "ms0:",   4)) { p += 4; sub = "ms";    }
    else if (!strncmp(p, "flash0:",7)) { p += 7; sub = "flash"; }
    else if (!strncmp(p, "host0:", 6)) { p += 6; sub = "host";  }

    while (*p == '/' || *p == '\\') p++;
    if (sub[0]) snprintf(out, cap, "%s/%s/%s", g_root, sub, p);
    else        snprintf(out, cap, "%s/%s",    g_root, p);
}

static void hle_Open(void) {
    char guest[512], host[1024];
    psp_str(psp_arg(0), guest, sizeof guest);
    uint32_t flags = psp_arg(1);
    map_path(guest, host, sizeof host);

    const char *mode = "rb";
    if (flags & PSP_O_TRUNC)                  mode = (flags & PSP_O_RDWR) == PSP_O_RDWR ? "w+b" : "wb";
    else if (flags & PSP_O_APPEND)            mode = "ab";
    else if ((flags & PSP_O_RDWR) == PSP_O_RDWR) mode = "r+b";
    else if (flags & PSP_O_WRONLY)            mode = (flags & PSP_O_CREAT) ? "wb" : "r+b";

    FILE *f = fopen(host, mode);
    if (!f && (flags & PSP_O_CREAT)) f = fopen(host, "w+b");
    /* PSPRECOMP_TRACE_IO=1 names every open and whether it worked. A game
     * that cannot find its assets usually does not crash; it waits. */
    if (getenv("PSPRECOMP_TRACE_IO"))
        fprintf(stderr, "io: open \"%s\" -> \"%s\" flags 0x%X: %s\n",
                guest, host, flags, f ? "ok" : "not found");
    if (!f) {
        /* A failed open is normal (a game probing for a save file) and is not
         * worth a warning, but the mapped path is worth knowing when a game
         * cannot find assets it expects. */
        psp_ret(0x80010002);          /* ENOENT */
        return;
    }

    for (int i = 0; i < MAX_FILES; i++) {
        if (g_file[i].used) continue;
        g_file[i].f = f;
        g_file[i].used = 1;
        psp_ret((uint32_t)(i + 3));   /* 0-2 are reserved for the std streams */
        return;
    }
    fclose(f);
    psp_ret(0x80010018);              /* too many open files */
}

static io_file *fd_arg(void) {
    int32_t fd = (int32_t)psp_arg(0) - 3;
    if (fd < 0 || fd >= MAX_FILES || !g_file[fd].used) return NULL;
    return &g_file[fd];
}

static void hle_Close(void) {
    io_file *h = fd_arg();
    if (!h) { psp_ret(0x80020323); return; }
    fclose(h->f);
    h->f = NULL;
    h->used = 0;
    psp_ret(0);
}

static void hle_Read(void) {
    io_file *h = fd_arg();
    uint32_t dst = psp_arg(1), size = psp_arg(2);
    if (!h) { psp_ret(0x80020323); return; }
    if (!size) { psp_ret(0); return; }

    /* Read through a host buffer and then place it, so a read that straddles
     * the end of a guest region is rejected by the memory layer rather than
     * writing past it. */
    uint8_t *tmp = (uint8_t *)malloc(size);
    if (!tmp) { psp_ret(0x80020190); return; }

    size_t got = fread(tmp, 1, size, h->f);
    if (got && psp_mem_write_block(dst, tmp, (uint32_t)got) != 0) {
        /* A destination that is not mapped end to end is a caller bug, and
         * writing the part that fits used to hide it: DJMAX Fever read 74 760
         * bytes into address 0, the byte-at-a-time fallback put them on top of
         * .text, and the game then walked that text as pointers. Refuse the
         * read and say so instead. */
        free(tmp);
        if (getenv("PSPRECOMP_TRACE_IO"))
            fprintf(stderr, "io: read %u bytes into unmapped 0x%08X: refused\n",
                    (unsigned)got, dst);
        psp_ret(0x80010005);              /* illegal address */
        return;
    }
    free(tmp);
    g_bytes_read += got;
    psp_ret((uint32_t)got);
}

static void hle_Write(void) {
    io_file *h = fd_arg();
    uint32_t src = psp_arg(1), size = psp_arg(2);

    /* fd 1 and 2 are stdout/stderr: a game writing there is talking to us. */
    int32_t fd = (int32_t)psp_arg(0);
    if (fd == 1 || fd == 2) {
        for (uint32_t i = 0; i < size; i++) fputc(psp_read8(src + i), stderr);
        psp_ret(size);
        return;
    }
    if (!h) { psp_ret(0x80020323); return; }

    uint8_t *tmp = (uint8_t *)malloc(size ? size : 1);
    if (!tmp) { psp_ret(0x80020190); return; }
    for (uint32_t i = 0; i < size; i++) tmp[i] = psp_read8(src + i);
    size_t put = fwrite(tmp, 1, size, h->f);
    free(tmp);
    psp_ret((uint32_t)put);
}

/* sceIoLseek takes a 64-bit offset and returns one. Under o32 a 64-bit
 * argument is register-aligned, so it lands in $a2:$a3 rather than $a1:$a2 --
 * and the result comes back in $v0:$v1. Getting either wrong makes every seek
 * land somewhere plausible but wrong. */
static void hle_Lseek(void) {
    io_file *h = fd_arg();
    uint64_t off = (uint64_t)psp_arg(2) | ((uint64_t)psp_arg(3) << 32);
    uint32_t whence = psp_arg(4);
    if (!h) { psp_ret(0x80020323); return; }

    int w = (whence == 1) ? SEEK_CUR : (whence == 2) ? SEEK_END : SEEK_SET;
    if (fseek(h->f, (long)(int64_t)off, w) != 0) { psp_ret(0xFFFFFFFFu); return; }

    long pos = ftell(h->f);
    psp_cpu.r[PSP_REG_V0] = (uint32_t)pos;
    psp_cpu.r[PSP_REG_V1] = 0;
}

static void hle_Rename(void) {
    char a[512], b[512], ha[1024], hb[1024];
    psp_str(psp_arg(0), a, sizeof a);
    psp_str(psp_arg(1), b, sizeof b);
    map_path(a, ha, sizeof ha);
    map_path(b, hb, sizeof hb);
    psp_ret(rename(ha, hb) == 0 ? 0 : 0x80010002);
}

/* ---- directories --------------------------------------------------------- */

static void hle_Dopen(void) {
    char guest[512], host[1024];
    psp_str(psp_arg(0), guest, sizeof guest);
    map_path(guest, host, sizeof host);

    for (int i = 0; i < MAX_DIRS; i++) {
        if (g_dir[i].used) continue;
#ifdef _WIN32
        char pattern[1088];
        snprintf(pattern, sizeof pattern, "%s/*", host);
        g_dir[i].handle = _findfirst(pattern, &g_dir[i].data);
        if (g_dir[i].handle == -1) { psp_ret(0x80010002); return; }
        g_dir[i].first = 1;
        g_dir[i].done = 0;
#else
        g_dir[i].dir = opendir(host);
        if (!g_dir[i].dir) { psp_ret(0x80010002); return; }
#endif
        g_dir[i].used = 1;
        psp_ret((uint32_t)(i + 1));
        return;
    }
    psp_ret(0x80010018);
}

/* SceIoDirent is SceIoStat (0x58) + char d_name[256] at 0x58 + void *d_private
 * at 0x158 + int dummy at 0x15C: 0x160 bytes.
 *
 * It used to be written as a 52-byte stat with the name at 52, which is wrong by
 * 0x24. The game then read the name out of the stat's *time* fields and used
 * chunks of it as pointers -- 0x6C5F6B38 is "8k_l" and 0x2E747369 is "ist.",
 * pieces of "4k_list.txt"/"8k_list.txt" from all.mp's table of contents. One run
 * spent 6.3 million bad reads walking that text. */
#define SCE_IO_STAT_SIZE      0x58
#define SCE_IO_NAME_OFFSET    0x58
#define SCE_IO_PRIVATE_OFFSET 0x158
#define SCE_IO_DIRENT_SIZE    0x160

static void write_stat(uint32_t at, int is_dir, long size);   /* defined below */

static void hle_Dread(void) {
    int32_t id = (int32_t)psp_arg(0) - 1;
    uint32_t dirent = psp_arg(1);
    if (id < 0 || id >= MAX_DIRS || !g_dir[id].used) { psp_ret(0x80020323); return; }

    const char *name = NULL;
    int   is_dir = 0;
    long  fsize  = 0;
#ifdef _WIN32
    if (g_dir[id].done) { psp_ret(0); return; }
    if (g_dir[id].first) {
        g_dir[id].first = 0;
        name = g_dir[id].data.name;
    } else if (_findnext(g_dir[id].handle, &g_dir[id].data) == 0) {
        name = g_dir[id].data.name;
    } else {
        g_dir[id].done = 1;
        psp_ret(0);
        return;
    }
    is_dir = (g_dir[id].data.attrib & _A_SUBDIR) != 0;
    fsize  = is_dir ? 0 : (long)g_dir[id].data.size;
#else
    struct dirent *de = readdir(g_dir[id].dir);
    if (!de) { psp_ret(0); return; }
    name = de->d_name;
#endif

    for (uint32_t i = 0; i < SCE_IO_DIRENT_SIZE; i++) psp_write8(dirent + i, 0);
    write_stat(dirent, is_dir, fsize);
    for (uint32_t i = 0; i < 255 && name[i]; i++)
        psp_write8(dirent + SCE_IO_NAME_OFFSET + i, (uint8_t)name[i]);

    psp_ret(1);                       /* more entries may follow */
}

static void hle_Dclose(void) {
    int32_t id = (int32_t)psp_arg(0) - 1;
    if (id < 0 || id >= MAX_DIRS || !g_dir[id].used) { psp_ret(0x80020323); return; }
#ifdef _WIN32
    if (g_dir[id].handle != -1) _findclose(g_dir[id].handle);
#else
    if (g_dir[id].dir) closedir(g_dir[id].dir);
#endif
    g_dir[id].used = 0;
    psp_ret(0);
}

/* ---- everything else -----------------------------------------------------
 *
 * The async family is what this game actually uses: `sceIoOpenAsync` was the
 * first call that returned a value the game dereferenced (it fed the result to
 * a `jalr` and jumped to 0x00000001). The host disk is instantaneous, so every
 * async call here performs the synchronous operation immediately and only the
 * *reporting* is deferred: `sceIoWaitAsync` hands back the stored result.
 *
 * Ids handed out for opens start at 0x1000, above any real fd (3..66), so one
 * table can serve both "the result of an open" and "the result of a read on fd
 * N" without the two colliding. */
#define MAX_ASYNC 64
static struct { uint32_t id; uint32_t result; int used; } g_async[MAX_ASYNC];
static uint32_t g_async_next = 0x1000;

static void async_store(uint32_t id, uint32_t result) {
    for (int i = 0; i < MAX_ASYNC; i++)
        if (g_async[i].used && g_async[i].id == id) { g_async[i].result = result; return; }
    for (int i = 0; i < MAX_ASYNC; i++)
        if (!g_async[i].used) { g_async[i].used = 1; g_async[i].id = id; g_async[i].result = result; return; }
}

static int32_t async_take(uint32_t id, uint32_t *out) {
    for (int i = 0; i < MAX_ASYNC; i++)
        if (g_async[i].used && g_async[i].id == id) {
            *out = g_async[i].result;
            g_async[i].used = 0;
            return 1;
        }
    return 0;
}

static void hle_OpenAsync(void) {
    hle_Open();                                   /* leaves fd or error in $v0 */
    uint32_t fd = psp_cpu.r[PSP_REG_V0];
    uint32_t id = g_async_next++;
    async_store(id, fd);
    if (getenv("PSPRECOMP_TRACE_IO"))
        fprintf(stderr, "io: openAsync id=0x%X -> 0x%08X\n", id, fd);
    psp_ret(id);
}

static void hle_WaitAsync(void) {
    uint32_t id = psp_arg(0), dst = psp_arg(1), result = 0;
    if (!async_take(id, &result)) result = (uint32_t)-1;   /* unknown id */
    if (dst) psp_write32(dst, result);
    psp_ret(0);                                   /* 0 == completed */
}

static void hle_PollAsync(void) {
    uint32_t id = psp_arg(0), dst = psp_arg(1), result = 0;
    int have = async_take(id, &result);
    if (dst) psp_write32(dst, have ? result : (uint32_t)-1);
    psp_ret(0);                                   /* everything is already done */
}

static void hle_ReadAsync(void) {
    hle_Read();                                   /* reads now, result in $v0 */
    async_store(psp_arg(0), psp_cpu.r[PSP_REG_V0]);
    psp_ret(0);
}

static void hle_CloseAsync(void) {
    hle_Close();
    psp_ret(0);
}

/* SceIoStat starts with st_mode (0x0010 directory, 0x0020 file), then
 * st_attr, st_size (64-bit), and the four time fields. Only mode, size and
 * "is a directory" are filled: the times are left at zero rather than invented,
 * and no game has been observed to branch on them yet. */
#define PSP_FIO_S_IFDIR 0x0010
#define PSP_FIO_S_IFREG 0x0020

static void write_stat(uint32_t at, int is_dir, long size) {
    for (int i = 0; i < 88; i++) psp_write8(at + (uint32_t)i, 0);
    psp_write32(at + 0,  is_dir ? PSP_FIO_S_IFDIR : PSP_FIO_S_IFREG);
    psp_write32(at + 4,  is_dir ? 0x0001 : 0x0020);   /* attr */
    psp_write32(at + 8,  (uint32_t)size);             /* st_size low */
    psp_write32(at + 12, 0);                          /* st_size high */
}

static void hle_Getstat(void) {
    char guest[512], host[1024];
    psp_str(psp_arg(0), guest, sizeof guest);
    map_path(guest, host, sizeof host);
    const uint32_t st = psp_arg(1);

    FILE *f = fopen(host, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        const long n = ftell(f);
        fclose(f);
        write_stat(st, 0, n);
        psp_ret(0);
        return;
    }
    /* Not a file: a directory is what the game is usually asking about. */
    {
        char probe[1088];
        snprintf(probe, sizeof probe, "%s/.", host);
        FILE *d = fopen(probe, "rb");
        if (d) { fclose(d); write_stat(st, 1, 0); psp_ret(0); return; }
    }
    psp_ret(0x80010002);                              /* ENOENT */
}

static void hle_Remove(void) {
    char g[512], h[1024];
    psp_str(psp_arg(0), g, sizeof g);
    map_path(g, h, sizeof h);
    psp_ret(remove(h) == 0 ? 0 : 0x80010002);
}

static void hle_Mkdir(void) {
    char g[512], h[1024];
    psp_str(psp_arg(0), g, sizeof g);
    map_path(g, h, sizeof h);
    psp_ret(_mkdir(h) == 0 ? 0 : 0x80010002);
}

static void hle_Rmdir(void) {
    char g[512], h[1024];
    psp_str(psp_arg(0), g, sizeof g);
    map_path(g, h, sizeof h);
    psp_ret(_rmdir(h) == 0 ? 0 : 0x80010002);
}

/* Anything the game asks the driver about (disc presence, read speed, the
 * "is this a UMD?" query). There is no driver here, so it is logged and
 * answered with OK -- the value is written to the caller's out pointer only
 * when the command is one we can answer, and zero otherwise rather than a
 * fabricated struct. */
static void hle_Devctl(void) {
    char g[512];
    psp_str(psp_arg(0), g, sizeof g);
    const uint32_t cmd = psp_arg(1), out = psp_arg(3), outlen = psp_arg(4);
    if (getenv("PSPRECOMP_TRACE_IO") || !outlen)
        fprintf(stderr, "io: devctl \"%s\" cmd=0x%08X out=0x%08X len=%u -> 0\n",
                g, cmd, out, outlen);
    (void)out;
    psp_ret(0);
}

static void hle_Ioctl(void) {
    const uint32_t cmd = psp_arg(1);
    if (getenv("PSPRECOMP_TRACE_IO"))
        fprintf(stderr, "io: ioctl cmd=0x%08X -> 0\n", cmd);
    psp_ret(0);
}

void psp_io_register(void) {
    psp_hle_register(0x109F50BC, "IoFileMgrForUser", "sceIoOpen",   hle_Open);
    psp_hle_register(0x810C4BC3, "IoFileMgrForUser", "sceIoClose",  hle_Close);
    psp_hle_register(0x6A638D83, "IoFileMgrForUser", "sceIoRead",   hle_Read);
    psp_hle_register(0x42EC03AC, "IoFileMgrForUser", "sceIoWrite",  hle_Write);
    psp_hle_register(0x27EB27B8, "IoFileMgrForUser", "sceIoLseek",  hle_Lseek);
    psp_hle_register(0x779103A0, "IoFileMgrForUser", "sceIoRename", hle_Rename);
    psp_hle_register(0xB29DDF9C, "IoFileMgrForUser", "sceIoDopen",  hle_Dopen);
    psp_hle_register(0xE3EB004C, "IoFileMgrForUser", "sceIoDread",  hle_Dread);
    psp_hle_register(0xEB092469, "IoFileMgrForUser", "sceIoDclose", hle_Dclose);

    /* Added in P5. Every name here hashes to its NID (SHA-1 of the name, first
     * four bytes little-endian), so tests/test_hle.c verifies them. */
    psp_hle_register(0xACE946E8, "IoFileMgrForUser", "sceIoGetstat", hle_Getstat);
    psp_hle_register(0xB8A740F4, "IoFileMgrForUser", "sceIoChstat",  hle_Ioctl);
    psp_hle_register(0xF27A9C51, "IoFileMgrForUser", "sceIoRemove",  hle_Remove);
    psp_hle_register(0x06A70004, "IoFileMgrForUser", "sceIoMkdir",   hle_Mkdir);
    psp_hle_register(0x1117C65F, "IoFileMgrForUser", "sceIoRmdir",   hle_Rmdir);
    psp_hle_register(0x54F5FB11, "IoFileMgrForUser", "sceIoDevctl",  hle_Devctl);
    psp_hle_register(0x63632449, "IoFileMgrForUser", "sceIoIoctl",   hle_Ioctl);
    psp_hle_register(0x89AA9906, "IoFileMgrForUser", "sceIoOpenAsync",  hle_OpenAsync);
    psp_hle_register(0xFF5940B6, "IoFileMgrForUser", "sceIoCloseAsync", hle_CloseAsync);
    psp_hle_register(0xA0B5A7C2, "IoFileMgrForUser", "sceIoReadAsync",  hle_ReadAsync);
    psp_hle_register(0xE23EEC33, "IoFileMgrForUser", "sceIoWaitAsync",  hle_WaitAsync);
    psp_hle_register(0x3251EA56, "IoFileMgrForUser", "sceIoPollAsync",  hle_PollAsync);
    psp_hle_register(0x35DBD746, "IoFileMgrForUser", "sceIoWaitAsyncCB", hle_WaitAsync);
    psp_hle_register(0xCB05F8D6, "IoFileMgrForUser", "sceIoGetAsyncStat", hle_PollAsync);
    psp_hle_register(0xA12A0514, "IoFileMgrForUser", "sceIoSetAsyncCallback", hle_Ioctl);
}
