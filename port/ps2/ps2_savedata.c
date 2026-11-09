/* ps2_savedata.c -- the PSP's savedata and message dialogs, on the PS2 memory card.
 *
 * sceUtilitySavedata: the game fills a SceUtilitySavedataParam and polls GetStatus
 * through INIT (1), VISIBLE (2), QUIT (3: it calls ShutdownStart), FINISHED (4), then NONE
 * (0), where it reads common.result (0x65490): 0, 0x80110307 (no data) or 0x80110301.
 * There is no dialog to show: the load or save happens at InitStart, the status walks the
 * same path. Data goes to /<gameName><saveName>/<fileName> on memory card 1 (BIOS
 * MCMAN/MCSERV through libmc, the card detected once at boot), so it works on PCSX2 and
 * on a console alike. The name "<>" (this game's auto save) adds nothing to the folder.
 * ponytail: only the data file is kept (no PARAM.SFO, icons or PS2 icon.sys: the PS2
 * browser shows the folder without an icon). List modes take the first name.
 *
 * sceUtilityMsgDialog: closes itself and answers YES (logged with its text), except
 * right after a failed save: then NO, or "try again?" would loop forever.
 *
 * Layouts as PPSSPP documents them (pspUtilityDialogCommon is 48 bytes, result at +28).
 */
#include "runtime_min.h"
#include "ps2_songs.h"

#include <fcntl.h>
#include <libmc.h>
#include <loadfile.h>
#include <sifrpc.h>
#include <string.h>
#include <unistd.h>

enum { DLG_NONE = 0, DLG_INIT, DLG_VISIBLE, DLG_QUIT, DLG_FINISHED };

#define SD_ERR_LOAD_NO_DATA  0x80110307u
#define SD_ERR_SAVE_FAILED   0x8011038Bu    /* "write failed" style; the game checks 0 only */
#define SD_ERR_NO_CARD       0x80110301u    /* load: no memory stick */

/* SceUtilitySavedataParam offsets */
enum { SD_MODE = 48, SD_GAMENAME = 60, SD_SAVENAME = 76, SD_NAMELIST = 96, SD_FILENAME = 100,
       SD_DATABUF = 116, SD_DATABUFSIZE = 120, SD_DATASIZE = 124 };

static int g_mc_ok, g_sd_failed;
static uint8_t g_io[24 * 1024] __attribute__((aligned(64)));   /* libmc transfers from here */

static int mc_wait(void) { int r = -1; mcSync(0, NULL, &r); return r; }

/* mcOpen takes the IOP's flags, not newlib's (whose O_RDONLY is 0: nothing readable). */
#define MC_RDONLY 0x0001
#define MC_WRONLY 0x0002
#define MC_CREAT  0x0200
#define MC_TRUNC  0x0400

void ps2_savedata_init(void) {
    const int a = SifLoadModule("rom0:MCMAN", 0, NULL);
    const int b = SifLoadModule("rom0:MCSERV", 0, NULL);
    int type = 0, freeb = 0, format = 0, r = -99;
    if (a >= 0 && b >= 0 && mcInit(MC_TYPE_MC) >= 0) {
        mcGetInfo(0, 0, &type, &freeb, &format);
        r = mc_wait();
        /* -2: a PS2 card that was never formatted (nothing on it to lose), as a new card
         * from PCSX2 or a shop. Formatted cards are never touched. */
        if (r == -2 && type == 2 && !format) {
            mcFormat(0, 0);
            const int f = mc_wait();
            mcGetInfo(0, 0, &type, &freeb, &format);
            r = mc_wait();
            psp_printf("[save] memory card 1 was unformatted: formatted (%d)\n", f);
        }
    }
    /* 0: same card, -1: a newly seen formatted card; below that, no usable card */
    g_mc_ok = r == 0 || r == -1;
    psp_printf("[save] memory card: mcman=%d mcserv=%d info=%d type=%d free=%d KB formatted=%d\n",
               a, b, r, type, freeb, format);
}

static void gstr(uint32_t addr, char *dst, size_t n) {
    const char *p = (const char *)psp_mem_ptr(addr, (uint32_t)n);
    size_t i = 0;
    if (p) for (; i + 1 < n && p[i]; i++) dst[i] = p[i];
    dst[i] = 0;
}

/* Keep only what a memory card file name allows. */
static void clean(char *s) {
    for (; *s; s++)
        if (!((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') ||
              *s == '_' || *s == '-' || *s == '.')) *s = '_';
}

static uint32_t g_sd_status, g_sd_param;

static void sd_paths(uint32_t p, const char *save, char *dir, char *file, size_t n) {
    char game[16], name[24];
    gstr(p + SD_GAMENAME, game, sizeof game);
    gstr(p + SD_FILENAME, name, 16);
    clean(game);
    char sv[24];
    strncpy(sv, strcmp(save, "<>") ? save : "", sizeof sv - 1);
    sv[sizeof sv - 1] = 0;
    clean(sv);
    clean(name);
    snprintf(dir, n, "/%s%s", game, sv);
    snprintf(file, n, "%s/%s", dir, name[0] ? name : "DATA.BIN");
}

static int file_exists(const char *path) {
    mcOpen(0, 0, path, MC_RDONLY);
    const int fd = mc_wait();
    if (fd < 0) return 0;
    mcClose(fd);
    mc_wait();
    return 1;
}

/* The save name to use: the param's, or for list modes the first of the list (for a
 * load, the first that exists). Written back into saveName as the dialog would. */
static void sd_pick(uint32_t p, uint32_t mode, char *save) {
    gstr(p + SD_SAVENAME, save, 20);
    const uint32_t list = psp_read32(p + SD_NAMELIST);
    if ((mode == 4u || mode == 5u || mode == 6u) && list) {
        char first[20] = "";
        for (uint32_t i = 0; i < 32u; i++) {
            char n[20];
            gstr(list + i * 20u, n, sizeof n);
            if (!n[0]) break;
            if (!first[0]) strcpy(first, n);
            if (mode == 4u) {
                char dir[64], file[96];
                sd_paths(p, n, dir, file, sizeof file);
                if (file_exists(file)) { strcpy(first, n); break; }
            }
        }
        if (first[0]) {
            strcpy(save, first);
            for (uint32_t i = 0; i < 20u; i++) psp_write8(p + SD_SAVENAME + i, i < strlen(first) ? (uint8_t)first[i] : 0);
        }
    }
}

/* Everything unlocked: applied to the game's save struct (the live one at 0x2CCF38, which
 * is also the save buffer) after a load and before every save. Layout from the code:
 * - songs: 60 records of 104 B at +392; byte 0 the song/MV, bytes 1-4 the 4B/5B/6B/8B
 *   modes; 0 locked, 1 new, 2 unlocked (0x8FFB4..0x902D4);
 * - collection at +6888: per type an array of 8 B entries {u8 locked, u8 new, u16, u32
 *   owned} (init 0x67FB4; the game's own unlock-all, 0x680D0, writes 0, 1, 1);
 * - +6884: 8B mode in normal difficulty (0x6FE8C).
 * Only locked things change, so nothing already seen turns "new" again. */
#define SD_UNLOCK_ALL 1
#define SD_GAME_SIZE  10868u
static void sd_unlock_all(uint32_t save) {
    uint8_t *s = (uint8_t *)psp_mem_ptr(save, SD_GAME_SIZE);
    if (!SD_UNLOCK_ALL || !s) return;
    for (unsigned id = 0; id < 60u; id++)
        for (unsigned k = 0; k < 5u; k++)
            if (!s[392u + 104u * id + k]) s[392u + 104u * id + k] = 2;
    /* gear, note, battler, image, media, music (none), ost1, ost2 */
    static const uint16_t off[8] = { 0, 160, 320, 752, 720, 0, 2672, 3136 };
    static const uint8_t cnt[8] = { 20, 20, 50, 240, 4, 0, 58, 101 };
    for (unsigned t = 0; t < 8u; t++)
        for (unsigned i = 0; i < cnt[t]; i++) {
            uint8_t *e = s + 6888u + off[t] + 8u * i;
            if (e[0]) { e[0] = 0; e[1] = 1; }
            if (!(e[4] | e[5] | e[6] | e[7])) e[4] = 1;
        }
    s[6884] = 1;
}

/* The records of the songs past 60 (ps2_songs.c) go in a file of their own next to the
 * game's, so SD.BIN keeps the PSP layout. Missing or of another size: fresh records. */
static void songs_path(const char *dir, char *out, size_t n) { snprintf(out, n, "%s/SONGS.BIN", dir); }

static void songs_load(const char *dir) {
    char path[96];
    uint32_t size;
    uint8_t *r = ps2_songs_extrec(&size);
    songs_path(dir, path, sizeof path);
    mcOpen(0, 0, path, MC_RDONLY);
    const int fd = mc_wait();
    int n = -1;
    if (fd >= 0) {
        if (r && size <= sizeof g_io) { mcRead(fd, g_io, (int)size); n = mc_wait(); }
        mcClose(fd);
        mc_wait();
    }
    if (n == (int)size) memcpy(r, g_io, size);
    else ps2_songs_extrec_reset();
    psp_printf("[save] %s: %s\n", path, n == (int)size ? "loaded" : "fresh records");
}

static void songs_save(const char *dir) {
    char path[96];
    uint32_t size;
    const uint8_t *r = ps2_songs_extrec(&size);
    if (!r || size > sizeof g_io) return;
    songs_path(dir, path, sizeof path);
    mcOpen(0, 0, path, MC_WRONLY | MC_CREAT | MC_TRUNC);
    const int fd = mc_wait();
    if (fd < 0) { psp_printf("[save] cannot create %s (%d)\n", path, fd); return; }
    memcpy(g_io, r, size);
    mcWrite(fd, g_io, (int)size);
    const int n = mc_wait();
    mcClose(fd);
    mc_wait();
    psp_printf("[save] %s: wrote %d of %u bytes\n", path, n, size);
}

static uint32_t sd_do(uint32_t p) {
    const uint32_t mode = psp_read32(p + SD_MODE);
    char save[24], dir[64], file[96], game[16], fname[16];
    sd_pick(p, mode, save);
    sd_paths(p, save, dir, file, sizeof file);
    gstr(p + SD_GAMENAME, game, sizeof game);
    gstr(p + SD_FILENAME, fname, sizeof fname);
    const uint32_t buf = psp_read32(p + SD_DATABUF), bufsize = psp_read32(p + SD_DATABUFSIZE);
    const uint32_t size = psp_read32(p + SD_DATASIZE);
    psp_printf("[save] mode=%u game=\"%s\" save=\"%s\" file=\"%s\" buf=0x%08X bufsize=%u size=%u -> %s\n",
               mode, game, save, fname, buf, bufsize, size, file);
    if (!g_mc_ok) return SD_ERR_NO_CARD;

    if (mode == 0u || mode == 2u || mode == 4u) {          /* AUTOLOAD, LOAD, LISTLOAD */
        mcOpen(0, 0, file, MC_RDONLY);
        const int fd = mc_wait();
        if (fd < 0) { psp_printf("[save] no data (%d)\n", fd); return SD_ERR_LOAD_NO_DATA; }
        uint8_t *dst = bufsize ? (uint8_t *)psp_mem_ptr(buf, bufsize) : NULL;
        int n = -1;
        if (dst && bufsize <= sizeof g_io) {
            mcRead(fd, g_io, (int)bufsize);
            n = mc_wait();
            if (n > 0) memcpy(dst, g_io, (size_t)n);
        }
        mcClose(fd);
        mc_wait();
        if (n < 0) { psp_printf("[save] read failed (%d)\n", n); return SD_ERR_LOAD_NO_DATA; }
        psp_write32(p + SD_DATASIZE, (uint32_t)n);
        psp_printf("[save] loaded %d bytes\n", n);
        if ((uint32_t)n == SD_GAME_SIZE) { sd_unlock_all(buf); songs_load(dir); }
        return 0;
    }
    if (mode == 1u || mode == 3u || mode == 5u) {          /* AUTOSAVE, SAVE, LISTSAVE */
        const uint8_t *src = size ? (const uint8_t *)psp_mem_ptr(buf, size) : NULL;
        if (!src || size > sizeof g_io) return SD_ERR_SAVE_FAILED;
        if (size == SD_GAME_SIZE) sd_unlock_all(buf);
        mcMkDir(0, 0, dir);
        const int md = mc_wait();                         /* -4: already there */
        mcOpen(0, 0, file, MC_WRONLY | MC_CREAT | MC_TRUNC);
        const int fd = mc_wait();
        if (fd < 0) { psp_printf("[save] cannot create %s (mkdir %d, open %d)\n", file, md, fd); return SD_ERR_SAVE_FAILED; }
        memcpy(g_io, src, size);
        mcWrite(fd, g_io, (int)size);
        const int n = mc_wait();
        mcClose(fd);
        mc_wait();
        psp_printf("[save] wrote %d of %u bytes\n", n, size);
        if (n == (int)size && size == SD_GAME_SIZE) songs_save(dir);
        return n == (int)size ? 0u : SD_ERR_SAVE_FAILED;
    }
    psp_printf("[save] mode %u not handled: reported done\n", mode);
    return 0;
}

/* A save shows "SAVING" (ps2_ge) for SAVE_LEAD_US before the write, which stalls the
 * game for a moment, and SAVE_TAIL frames after it, like the PSP's own save dialog. */
#define SAVE_LEAD_US 500000ull
#define SAVE_TAIL    60
void ps2_ge_osd_saving(int frames);
uint64_t ps2_time_us(void);
static int g_sd_pending;
static uint64_t g_sd_due;

static int is_save(uint32_t mode) { return mode == 1u || mode == 3u || mode == 5u; }

static void sd_finish(void) {
    const uint32_t r = sd_do(g_sd_param);
    g_sd_failed = r != 0 && is_save(psp_read32(g_sd_param + SD_MODE));
    if (psp_mem_ptr(g_sd_param, 32)) psp_write32(g_sd_param + 28, r);
}

static void hle_SavedataInitStart(void) {
    if (g_sd_status != DLG_NONE) { psp_ret(0x80110001u); return; }   /* busy */
    g_sd_param = psp_arg(0);
    g_sd_pending = 0;
    if (g_mc_ok && is_save(psp_read32(g_sd_param + SD_MODE))) {
        g_sd_pending = 1;
        g_sd_due = ps2_time_us() + SAVE_LEAD_US;
        ps2_ge_osd_saving(1000000);                 /* until the write is done */
    } else {
        sd_finish();
    }
    g_sd_status = DLG_INIT;
    psp_ret(0);
}
static void hle_SavedataGetStatus(void) {
    const uint32_t s = g_sd_status;
    if (s == DLG_INIT) g_sd_status = DLG_VISIBLE;
    else if (s == DLG_VISIBLE) {
        if (g_sd_pending && ps2_time_us() < g_sd_due) { psp_ret(s); return; }
        if (g_sd_pending) { sd_finish(); g_sd_pending = 0; ps2_ge_osd_saving(SAVE_TAIL); }
        g_sd_status = DLG_QUIT;
    }
    else if (s == DLG_FINISHED) g_sd_status = DLG_NONE;
    psp_ret(s);
}
static void hle_SavedataShutdownStart(void) {
    if (g_sd_status != DLG_QUIT) { psp_ret(0x80110001u); return; }
    g_sd_status = DLG_FINISHED;
    psp_ret(0);
}
static void hle_DialogUpdate(void) { psp_ret(0); }

/* pspUtilityMsgDialogParams: mode +52, errorValue +56, message[512] +60, options +572,
 * buttonPressed +576 (1 = YES). */
static uint32_t g_md_status, g_md_param;
static void hle_MsgDialogInitStart(void) {
    if (g_md_status != DLG_NONE) { psp_ret(0x80110001u); return; }
    g_md_param = psp_arg(0);
    char msg[96];
    gstr(g_md_param + 60, msg, sizeof msg);
    for (char *c = msg; *c; c++) if (*c == '\n' || *c == '\r') *c = ' ';
    const uint32_t answer = g_sd_failed ? 2u : 1u;          /* 1 YES, 2 NO */
    psp_printf("[msgdialog] mode=%u error=0x%08X options=0x%X \"%s\" -> %s\n", psp_read32(g_md_param + 52),
               psp_read32(g_md_param + 56), psp_read32(g_md_param + 572), msg, answer == 1u ? "YES" : "NO");
    if (psp_mem_ptr(g_md_param, 580)) {
        psp_write32(g_md_param + 28, 0);
        psp_write32(g_md_param + 576, answer);
    }
    g_md_status = DLG_INIT;
    psp_ret(0);
}
static void hle_MsgDialogGetStatus(void) {
    const uint32_t s = g_md_status;
    if (s == DLG_INIT) g_md_status = DLG_VISIBLE;
    else if (s == DLG_VISIBLE) g_md_status = DLG_QUIT;
    else if (s == DLG_FINISHED) g_md_status = DLG_NONE;
    psp_ret(s);
}
static void hle_MsgDialogShutdownStart(void) {
    if (g_md_status != DLG_QUIT) { psp_ret(0x80110001u); return; }
    g_md_status = DLG_FINISHED;
    psp_ret(0);
}

void ps2_savedata_install(void) {
    psp_hle_register(0x50C4CD57, "sceUtility", "sceUtilitySavedataInitStart",     hle_SavedataInitStart);
    psp_hle_register(0x8874DBE0, "sceUtility", "sceUtilitySavedataGetStatus",     hle_SavedataGetStatus);
    psp_hle_register(0xD4B95FFB, "sceUtility", "sceUtilitySavedataUpdate",        hle_DialogUpdate);
    psp_hle_register(0x9790B33C, "sceUtility", "sceUtilitySavedataShutdownStart", hle_SavedataShutdownStart);
    psp_hle_register(0x2AD8E239, "sceUtility", "sceUtilityMsgDialogInitStart",    hle_MsgDialogInitStart);
    psp_hle_register(0x9A1C91D7, "sceUtility", "sceUtilityMsgDialogGetStatus",    hle_MsgDialogGetStatus);
    psp_hle_register(0x95FC253B, "sceUtility", "sceUtilityMsgDialogUpdate",       hle_DialogUpdate);
    psp_hle_register(0x67AF3428, "sceUtility", "sceUtilityMsgDialogShutdownStart", hle_MsgDialogShutdownStart);
}
