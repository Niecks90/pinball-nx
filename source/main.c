#include <switch.h>
#include <errno.h>
#include <malloc.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "font5x7.h"
#include "gbc.h"
#include "pinball.h"

#define SCREEN_W        1280
#define SCREEN_H        720
#define SWITCH_DIR      "sdmc:/switch"
#define BASE_DIR        "sdmc:/switch/pinball-nx"
#define ROM_PATH        BASE_DIR "/pokemon_pinball.gbc"
#define GEN_ROM_PATH    BASE_DIR "/pokemon_pinball_generations.gbc"
#define GEN_IPS_PATH    BASE_DIR "/pokemon_pinball_generations.ips"
#define GEN_IPS_ALT     BASE_DIR "/generations.ips"
#define SAV_ORIG_PATH   BASE_DIR "/pinball.sav"
#define SAV_GEN_PATH    BASE_DIR "/gen_pinball.sav"
#define CFG_PATH        BASE_DIR "/config.ini"
#define LOG_PATH        BASE_DIR "/boot.log"

#define AUD_SAMPLE_RATE 48000u
#define AUD_CHANNELS    2u
#define AUD_BUF_FRAMES  1024u
#define AUD_BUF_BYTES   (AUD_BUF_FRAMES * AUD_CHANNELS * sizeof(int16_t))
#define AUD_NUM_BUFS    4
#define MAX_ROM_BYTES   (16u * 1024u * 1024u)

/* Request a 1 MiB main thread stack on Horizon OS; all large state lives on the heap. */
size_t __nx_main_thread_stack_size = 1024 * 1024;

typedef enum {
    ORIENTATION_LANDSCAPE = 0,
    ORIENTATION_PORTRAIT_CW = 1,
    ORIENTATION_PORTRAIT_CCW = 2,
} Orientation;

typedef struct {
    Orientation orientation;
    bool show_touch_guide;
    bool landscape_follow;
    int generation; /* PB_ROM_ORIGINAL (0) or PB_ROM_GENERATIONS (1) */
} Config;

typedef struct {
    float y;
    float prev_ball_y;
    float vel_y;
    bool has_ball;
    int last_h;
    bool last_in_game;
} LandscapeCam;

static PadState g_pad;
static Framebuffer g_fb;
static bool g_fb_ok = false;

static Config g_config = { ORIENTATION_LANDSCAPE, true, true, PB_ROM_ORIGINAL };
static uint32_t *g_frame = NULL;
static size_t g_frame_pixels = 0;

static LandscapeCam g_cam;

static AudioOutBuffer g_aud_bufs[AUD_NUM_BUFS];
static int16_t *g_aud_pcm[AUD_NUM_BUFS];
static bool g_aud_in_flight[AUD_NUM_BUFS];
static bool g_aud_started = false;
static bool g_aud_init_done = false;
static bool g_aud_ok = false;

static bool g_boot_log_started = false;
static char g_rom_error[256] = "";
static char g_pause_status[96] = "";
static int g_pause_status_frames = 0;

static char g_rom_paths[2][256];
static bool g_rom_avail[2] = { false, false };
static uint8_t g_sav_last[0x8000];
static size_t g_sav_last_size = 0;

static void log_boot(const char *fmt, ...)
{
    const char *mode = g_boot_log_started ? "a" : "w";
    FILE *f = fopen(LOG_PATH, mode);
    if (!f) return;
    g_boot_log_started = true;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);
    fclose(f);
}

#define LOG_BOOT(...) log_boot(__VA_ARGS__)

static inline uint32_t gbc_to_rgba8(uint32_t c)
{
    return ((c & 0x00FF0000u) >> 16) |
           (c & 0x0000FF00u) |
           ((c & 0x000000FFu) << 16) |
           0xFF000000u;
}

static bool config_save(void)
{
    FILE *f = fopen(CFG_PATH, "wb");
    if (!f) return false;
    fprintf(f,
            "orientation=%d\ntouch_guide=%d\nlandscape_camera=%d\ngeneration=%d\n",
            (int)g_config.orientation,
            g_config.show_touch_guide ? 1 : 0,
            g_config.landscape_follow ? 1 : 0,
            g_config.generation == PB_ROM_GENERATIONS ? 1 : 0);
    fflush(f);
    fclose(f);
    return true;
}

static void config_load_or_create(void)
{
    FILE *f = fopen(CFG_PATH, "rb");
    if (!f) {
        bool created = config_save();
        LOG_BOOT("config.ini created (%s): orientation=%d touch_guide=%d landscape_camera=%d generation=%d",
                 created ? "ok" : "write_failed",
                 (int)g_config.orientation,
                 g_config.show_touch_guide ? 1 : 0,
                 g_config.landscape_follow ? 1 : 0,
                 g_config.generation);
        return;
    }

    char line[64];
    while (fgets(line, sizeof line, f)) {
        int v = 0;
        if (sscanf(line, "orientation=%d", &v) == 1 && v >= 0 && v <= 2)
            g_config.orientation = (Orientation)v;
        if (sscanf(line, "touch_guide=%d", &v) == 1)
            g_config.show_touch_guide = (v != 0);
        if (sscanf(line, "landscape_camera=%d", &v) == 1)
            g_config.landscape_follow = (v != 0);
        if (sscanf(line, "generation=%d", &v) == 1 && (v == 0 || v == 1))
            g_config.generation = v;
    }
    fclose(f);
    LOG_BOOT("config.ini loaded: orientation=%d touch_guide=%d landscape_camera=%d generation=%d",
             (int)g_config.orientation,
             g_config.show_touch_guide ? 1 : 0,
             g_config.landscape_follow ? 1 : 0,
             g_config.generation);
}

static uint8_t *read_file_heap(const char *path, size_t *size, size_t max_bytes)
{
    *size = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n <= 0 || (size_t)n > max_bytes) {
        fclose(f);
        return NULL;
    }
    rewind(f);

    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

static bool write_file_all(const char *path, const void *data, size_t size)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, size, f) == size;
    if (fflush(f) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    return ok;
}

/* Apply an IPS patch ("PATCH" ... "EOF") to a US ROM buffer on the heap. */
static uint8_t *ips_apply(const uint8_t *rom, size_t rom_size, const uint8_t *ips, size_t n, size_t *out_size)
{
    const size_t max = 8u << 20;
    if (n < 8 || memcmp(ips, "PATCH", 5) != 0 || rom_size > max) return NULL;
    uint8_t *out = (uint8_t *)calloc(1, max);
    if (!out) return NULL;
    memcpy(out, rom, rom_size);
    size_t size = rom_size;
    size_t i = 5;
    for (;;) {
        if (i + 3 > n) { free(out); return NULL; }
        if (!memcmp(ips + i, "EOF", 3)) {
            i += 3;
            if (i + 3 <= n)
                size = ((size_t)ips[i] << 16) | ((size_t)ips[i + 1] << 8) | ips[i + 2];
            break;
        }
        if (i + 5 > n) { free(out); return NULL; }
        size_t off = ((size_t)ips[i] << 16) | ((size_t)ips[i + 1] << 8) | ips[i + 2];
        size_t len = ((size_t)ips[i + 3] << 8) | ips[i + 4];
        i += 5;
        if (len) {
            if (i + len > n || off + len > max) { free(out); return NULL; }
            memcpy(out + off, ips + i, len);
            i += len;
        } else {
            if (i + 3 > n) { free(out); return NULL; }
            len = ((size_t)ips[i] << 8) | ips[i + 1];
            if (off + len > max) { free(out); return NULL; }
            memset(out + off, ips[i + 2], len);
            i += 3;
        }
        if (off + len > size) size = off + len;
    }
    *out_size = size;
    return out;
}

static void probe_rom_file(const char *path, char *first_invalid_sha, size_t sha_cap)
{
    size_t n = 0;
    uint8_t *data = read_file_heap(path, &n, MAX_ROM_BYTES);
    if (!data) return;

    char sha[41];
    pb_sha1_hex(data, n, sha);
    free(data);

    const PbRom *prof = pb_rom_profile(sha);
    if (prof && (prof->id == PB_ROM_ORIGINAL || prof->id == PB_ROM_GENERATIONS)) {
        if (!g_rom_avail[prof->id]) {
            snprintf(g_rom_paths[prof->id], sizeof(g_rom_paths[prof->id]), "%s", path);
            g_rom_avail[prof->id] = true;
            LOG_BOOT("ROM discovered [%s]: %s (%zu bytes, SHA-1 %.12s)",
                     prof->name, path, n, sha);
        }
    } else if (first_invalid_sha && !first_invalid_sha[0]) {
        snprintf(first_invalid_sha, sha_cap, "%s", sha);
        LOG_BOOT("ROM unrecognized at %s (SHA-1 %s)", path, sha);
    }
}

static void discover_roms(void)
{
    memset(g_rom_paths, 0, sizeof(g_rom_paths));
    g_rom_avail[0] = g_rom_avail[1] = false;

    char invalid_sha[41] = "";
    probe_rom_file(ROM_PATH, invalid_sha, sizeof(invalid_sha));
    probe_rom_file(GEN_ROM_PATH, invalid_sha, sizeof(invalid_sha));

    if (!g_rom_avail[PB_ROM_GENERATIONS] && g_rom_avail[PB_ROM_ORIGINAL]) {
        const char *ips_candidates[2] = { GEN_IPS_PATH, GEN_IPS_ALT };
        for (int k = 0; k < 2 && !g_rom_avail[PB_ROM_GENERATIONS]; k++) {
            size_t ips_sz = 0;
            uint8_t *ips = read_file_heap(ips_candidates[k], &ips_sz, MAX_ROM_BYTES);
            if (!ips) continue;
            size_t us_sz = 0;
            uint8_t *us = read_file_heap(g_rom_paths[PB_ROM_ORIGINAL], &us_sz, MAX_ROM_BYTES);
            if (us) {
                size_t gen_sz = 0;
                uint8_t *gen = ips_apply(us, us_sz, ips, ips_sz, &gen_sz);
                free(us);
                if (gen) {
                    char sha[41];
                    pb_sha1_hex(gen, gen_sz, sha);
                    const PbRom *prof = pb_rom_profile(sha);
                    if (prof && prof->id == PB_ROM_GENERATIONS) {
                        if (write_file_all(GEN_ROM_PATH, gen, gen_sz)) {
                            snprintf(g_rom_paths[PB_ROM_GENERATIONS], sizeof(g_rom_paths[PB_ROM_GENERATIONS]),
                                     "%s", GEN_ROM_PATH);
                            g_rom_avail[PB_ROM_GENERATIONS] = true;
                            LOG_BOOT("Generations IPS applied from %s -> cached %s", ips_candidates[k], GEN_ROM_PATH);
                        }
                    } else {
                        LOG_BOOT("Generations IPS patch %s produced unrecognized SHA-1 %s", ips_candidates[k], sha);
                    }
                    free(gen);
                }
            }
            free(ips);
        }
    }

    if (!g_rom_avail[PB_ROM_ORIGINAL] && !g_rom_avail[PB_ROM_GENERATIONS] && invalid_sha[0]) {
        snprintf(g_rom_error, sizeof(g_rom_error),
                 "Unrecognized ROM (SHA-1 %.12s...). Expected Pokemon Pinball (US) or Generations 1.6.",
                 invalid_sha);
    }
}

static const char *sram_path_for_gen(int gen)
{
    return (gen == PB_ROM_GENERATIONS) ? SAV_GEN_PATH : SAV_ORIG_PATH;
}

static void sram_load(Pinball *pb)
{
    if (!pb) return;
    const char *path = sram_path_for_gen(pb_rom_id(pb));
    size_t n = 0;
    uint8_t *d = read_file_heap(path, &n, sizeof(g_sav_last));
    if (d) {
        pb_load_sram(pb, d, n);
        free(d);
    }
    const uint8_t *s = pb_sram(pb, &g_sav_last_size);
    if (g_sav_last_size > sizeof(g_sav_last)) g_sav_last_size = sizeof(g_sav_last);
    if (g_sav_last_size > 0) memcpy(g_sav_last, s, g_sav_last_size);
}

static void sram_flush(Pinball *pb)
{
    if (!pb) return;
    size_t n = 0;
    const uint8_t *s = pb_sram(pb, &n);
    if (n == 0 || n > sizeof(g_sav_last)) return;
    if (n == g_sav_last_size && memcmp(s, g_sav_last, n) == 0) return;
    if (write_file_all(sram_path_for_gen(pb_rom_id(pb)), s, n)) {
        memcpy(g_sav_last, s, n);
        g_sav_last_size = n;
    }
}

static void camera_reset(void)
{
    memset(&g_cam, 0, sizeof(g_cam));
}

static bool start_emulator_for_gen(int desired_gen, Pinball **pb_out, uint8_t **rom_out, size_t *rom_size_out)
{
    int gen = desired_gen;
    if (gen < 0 || gen > 1 || !g_rom_avail[gen]) {
        if (g_rom_avail[PB_ROM_ORIGINAL]) gen = PB_ROM_ORIGINAL;
        else if (g_rom_avail[PB_ROM_GENERATIONS]) gen = PB_ROM_GENERATIONS;
        else return false;
    }

    size_t rom_size = 0;
    uint8_t *rom = read_file_heap(g_rom_paths[gen], &rom_size, MAX_ROM_BYTES);
    if (!rom) {
        snprintf(g_rom_error, sizeof(g_rom_error), "Could not read ROM file: %.200s", g_rom_paths[gen]);
        LOG_BOOT("ROM load failed: %s", g_rom_paths[gen]);
        return false;
    }

    Pinball *pb = (Pinball *)calloc(1, sizeof(Pinball));
    if (!pb) {
        snprintf(g_rom_error, sizeof(g_rom_error), "Out of memory allocating emulator state.");
        LOG_BOOT("emulator init: FAILED (calloc Pinball %zu bytes failed)", sizeof(Pinball));
        free(rom);
        return false;
    }

    if (pb_init(pb, rom, rom_size, AUD_SAMPLE_RATE) != 0) {
        snprintf(g_rom_error, sizeof(g_rom_error), "%s",
                 pb->error[0] ? pb->error : "Unsupported or invalid ROM.");
        LOG_BOOT("emulator init: FAILED (%s)", g_rom_error);
        pb_free(pb);
        free(pb);
        free(rom);
        return false;
    }

    pb_set_narrow(pb, true);
    pb_set_antilag(pb, true);
    pb_set_double_speed(pb, true);
    pb_set_frame_palettes(pb, true);
    pb_set_hold(pb, true);
    pb_set_audio_highpass(pb, GBC_HIGHPASS_FRONT);
    sram_load(pb);
    camera_reset();

    if (g_config.generation != pb_rom_id(pb)) {
        g_config.generation = pb_rom_id(pb);
        config_save();
    }

    *pb_out = pb;
    *rom_out = rom;
    *rom_size_out = rom_size;
    LOG_BOOT("emulator init: ok (%s, %zu bytes)", pb_rom_name(pb), rom_size);
    return true;
}

static void clear_frame(uint32_t color)
{
    if (!g_frame) return;
    for (size_t i = 0; i < g_frame_pixels; i++)
        g_frame[i] = color;
}

static void fill_rect(int x, int y, int w, int h, uint32_t color)
{
    if (!g_frame) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCREEN_W) w = SCREEN_W - x;
    if (y + h > SCREEN_H) h = SCREEN_H - y;
    if (w <= 0 || h <= 0) return;

    uint32_t a = (color >> 24) & 0xFFu;
    if (a == 0xFFu) {
        for (int yy = y; yy < y + h; yy++) {
            uint32_t *row = &g_frame[(size_t)yy * SCREEN_W + x];
            for (int xx = 0; xx < w; xx++)
                row[xx] = color;
        }
    } else if (a > 0) {
        uint32_t sr = color & 0xFFu;
        uint32_t sg = (color >> 8) & 0xFFu;
        uint32_t sb = (color >> 16) & 0xFFu;
        uint32_t inv = 255u - a;
        for (int yy = y; yy < y + h; yy++) {
            uint32_t *row = &g_frame[(size_t)yy * SCREEN_W + x];
            for (int xx = 0; xx < w; xx++) {
                uint32_t dst = row[xx];
                uint32_t dr = ((sr * a) + ((dst & 0xFFu) * inv)) / 255u;
                uint32_t dg = ((sg * a) + (((dst >> 8) & 0xFFu) * inv)) / 255u;
                uint32_t db = ((sb * a) + (((dst >> 16) & 0xFFu) * inv)) / 255u;
                row[xx] = RGBA8_MAXALPHA(dr, dg, db);
            }
        }
    }
}

static inline int virt_w(void)
{
    return (g_config.orientation == ORIENTATION_LANDSCAPE) ? SCREEN_W : SCREEN_H;
}

static inline int virt_h(void)
{
    return (g_config.orientation == ORIENTATION_LANDSCAPE) ? SCREEN_H : SCREEN_W;
}

static void vfill_rect(int vx, int vy, int rw, int rh, uint32_t color)
{
    if (g_config.orientation == ORIENTATION_LANDSCAPE) {
        fill_rect(vx, vy, rw, rh, color);
    } else if (g_config.orientation == ORIENTATION_PORTRAIT_CW) {
        fill_rect(vy, SCREEN_H - (vx + rw), rh, rw, color);
    } else {
        fill_rect(SCREEN_W - (vy + rh), vx, rh, rw, color);
    }
}

static void vstroke_rect(int vx, int vy, int rw, int rh, int t, uint32_t color)
{
    vfill_rect(vx, vy, rw, t, color);
    vfill_rect(vx, vy + rh - t, rw, t, color);
    vfill_rect(vx, vy, t, rh, color);
    vfill_rect(vx + rw - t, vy, t, rh, color);
}

static int vtext_width(const char *text, int scale)
{
    if (!text || !*text || scale < 1) return 0;
    int len = utf8_len(text);
    return len > 0 ? (len * FONT_CELL_W - 1) * scale : 0;
}

static void vdraw_text(int vx, int vy, int scale, uint32_t color, const char *text)
{
    if (!g_frame || !text || scale < 1) return;
    font_init();
    int cx = vx;
    const char *p = text;
    while (*p) {
        if (*p == '\n') {
            cx = vx;
            vy += FONT_CELL_H * scale;
            p++;
            continue;
        }
        uint32_t cp = utf8_next(&p);
        const FontGlyph *g = font_glyph(cp);
        for (int r = 0; r < FONT_H; r++) {
            uint8_t bits = g->rows[r];
            for (int c = 0; c < FONT_W; c++) {
                if (bits & (0x10 >> c))
                    vfill_rect(cx + c * scale, vy + r * scale, scale, scale, color);
            }
        }
        cx += FONT_CELL_W * scale;
    }
}

static void vdraw_text_centered(int vcx, int vcy, int scale, uint32_t color, const char *text)
{
    int tw = vtext_width(text, scale);
    int th = FONT_H * scale;
    vdraw_text(vcx - tw / 2, vcy - th / 2, scale, color, text);
}

static void vdraw_text_wrapped(int vx, int vy, int max_w, int scale, uint32_t color, const char *text)
{
    if (!text || scale < 1) return;
    int max_cols = max_w / (FONT_CELL_W * scale);
    if (max_cols < 12) max_cols = 12;

    char line[128];
    const char *p = text;
    while (*p) {
        int len = 0;
        int last_space = -1;
        while (p[len] && p[len] != '\n' && len < max_cols && len < (int)sizeof(line) - 1) {
            if (p[len] == ' ') last_space = len;
            len++;
        }
        if (p[len] && p[len] != '\n' && last_space > 0)
            len = last_space;
        memcpy(line, p, (size_t)len);
        line[len] = '\0';
        vdraw_text(vx, vy, scale, color, line);
        vy += FONT_CELL_H * scale + 4;
        p += len;
        while (*p == ' ') p++;
        if (*p == '\n') p++;
    }
}

static const char *generation_short_label(int gen)
{
    return (gen == PB_ROM_GENERATIONS) ? "GEN 1.6" : "GEN 1";
}

static const char *generation_full_label(int gen)
{
    return (gen == PB_ROM_GENERATIONS) ? "GENERATIONS 1.6 (6 TABLES)" : "ORIGINAL (GEN 1 / RED & BLUE)";
}

static const char *table_name_str(const Pinball *pb)
{
    if (!pb) return "";
    int t = pb_table(pb);
    switch (t) {
    case PB_TABLE_RED:      return "RED TABLE";
    case PB_TABLE_BLUE:     return "BLUE TABLE";
    case PB_TABLE_GOLD:     return "GOLD TABLE";
    case PB_TABLE_SILVER:   return "SILVER TABLE";
    case PB_TABLE_RUBY:     return "RUBY TABLE";
    case PB_TABLE_SAPPHIRE: return "SAPPHIRE TABLE";
    default: break;
    }
    int st = pb_stage(pb);
    if (st >= 0) {
        switch (pb_stage_bonus(pb, st)) {
        case PB_BONUS_GENGAR:  return "GENGAR BONUS";
        case PB_BONUS_MEWTWO:  return "MEWTWO BONUS";
        case PB_BONUS_MEOWTH:  return "MEOWTH BONUS";
        case PB_BONUS_DIGLETT: return "DIGLETT BONUS";
        case PB_BONUS_SEEL:    return "SEEL BONUS";
        default: break;
        }
    }
    return "MENU / TITLE";
}

static void draw_touch_zone_box(int vx, int vy, int w, int h, uint32_t bg, uint32_t border,
                                int text_scale, const char *label)
{
    vfill_rect(vx, vy, w, h, bg);
    vstroke_rect(vx, vy, w, h, 2, border);
    vdraw_text_centered(vx + w / 2, vy + h / 2, text_scale, RGBA8_MAXALPHA(245, 248, 255), label);
}

static void draw_touch_guide(void)
{
    if (!g_config.show_touch_guide) return;

    int vw = virt_w();
    int vh = virt_h();
    uint32_t blue_bg = RGBA8(44, 78, 140, 92);
    uint32_t blue_bd = RGBA8(110, 165, 240, 140);
    uint32_t green_bg = RGBA8(52, 120, 72, 96);
    uint32_t green_bd = RGBA8(120, 210, 140, 140);
    uint32_t amber_bg = RGBA8(120, 88, 36, 86);
    uint32_t amber_bd = RGBA8(220, 180, 96, 130);
    uint32_t gray_bg = RGBA8(72, 80, 96, 110);
    uint32_t gray_bd = RGBA8(170, 182, 204, 150);

    if (g_config.orientation == ORIENTATION_LANDSCAPE) {
        draw_touch_zone_box(16, vh - 160, 260, 144, blue_bg, blue_bd, 3, "L FLIPPER");
        draw_touch_zone_box(vw - 276, vh - 160, 260, 144, blue_bg, blue_bd, 3, "R FLIPPER");
        draw_touch_zone_box(vw / 2 - 110, vh - 86, 220, 74, green_bg, green_bd, 2, "PLUNGER");
        draw_touch_zone_box(16, 124, 210, 68, amber_bg, amber_bd, 2, "NUDGE L");
        draw_touch_zone_box(vw / 2 - 90, 12, 180, 44, amber_bg, amber_bd, 2, "NUDGE UP");
        draw_touch_zone_box(vw - 226, 100, 210, 68, amber_bg, amber_bd, 2, "NUDGE R");
        draw_touch_zone_box(vw - 150, 12, 134, 60, gray_bg, gray_bd, 2, "PAUSE");
    } else {
        draw_touch_zone_box(12, 12, 176, 48, amber_bg, amber_bd, 2, "NUDGE L");
        draw_touch_zone_box(vw / 2 - 110, 12, 200, 48, amber_bg, amber_bd, 2, "NUDGE UP");
        draw_touch_zone_box(vw - 144, 12, 132, 48, gray_bg, gray_bd, 2, "PAUSE");
        draw_touch_zone_box(vw - 188, 68, 176, 48, amber_bg, amber_bd, 2, "NUDGE R");
        draw_touch_zone_box(12, vh - 98, 212, 86, blue_bg, blue_bd, 2, "L FLIP");
        draw_touch_zone_box(vw / 2 - 108, vh - 98, 216, 86, green_bg, green_bd, 2, "PLUNGER");
        draw_touch_zone_box(vw - 224, vh - 98, 212, 86, blue_bg, blue_bd, 2, "R FLIP");
    }
}

static void draw_side_hud_landscape(const Pinball *pb, int panel_w)
{
    if (!pb || panel_w < 170) return;

    int bx = 16;
    int by = 14;
    int bw = panel_w - 32;
    if (bw > 260) bw = 260;

    vfill_rect(bx, by, bw, 94, RGBA8(22, 32, 46, 215));
    vstroke_rect(bx, by, bw, 94, 2, RGBA8_MAXALPHA(72, 102, 140));
    vdraw_text(bx + 12, by + 10, 2, RGBA8_MAXALPHA(242, 193, 78), "PINBALL NX");
    vdraw_text(bx + 12, by + 34, 2, RGBA8_MAXALPHA(210, 232, 255), generation_short_label(pb_rom_id(pb)));
    vdraw_text(bx + 12, by + 56, 2, RGBA8_MAXALPHA(170, 190, 212), table_name_str(pb));
    vdraw_text(bx + 12, by + 76, 1, RGBA8_MAXALPHA(135, 165, 195),
               g_config.landscape_follow ? "CAM: FOLLOW BALL  [+] MENU" : "CAM: FULL TABLE   [+] MENU");
}

static void draw_missing_rom(void)
{
    int vw = virt_w();
    int vh = virt_h();
    int margin = 56;
    int box_w = vw - margin * 2;
    int box_h = vh - margin * 2;

    clear_frame(RGBA8_MAXALPHA(14, 18, 24));
    vfill_rect(margin, margin, box_w, box_h, RGBA8_MAXALPHA(26, 36, 50));
    vfill_rect(margin, margin, box_w, 6, RGBA8_MAXALPHA(242, 193, 78));

    int tx = margin + 36;
    int tw = box_w - 72;
    int ty = margin + 36;
    vdraw_text(tx, ty, 4, RGBA8_MAXALPHA(255, 255, 255), "PINBALL NX");
    ty += 54;

    if (g_rom_error[0]) {
        vdraw_text(tx, ty, 3, RGBA8_MAXALPHA(255, 130, 110), "ROM COULD NOT BE LOADED");
        ty += 42;
        vdraw_text_wrapped(tx, ty, tw, 2, RGBA8_MAXALPHA(230, 236, 244), g_rom_error);
    } else {
        vdraw_text(tx, ty, 3, RGBA8_MAXALPHA(242, 193, 78), "POKEMON PINBALL ROM REQUIRED");
        ty += 44;
        vdraw_text_wrapped(tx, ty, tw, 2, RGBA8_MAXALPHA(220, 228, 238),
                           "PLACE YOUR COMPATIBLE US ROM (AND OPTIONAL GENERATIONS 1.6 ROM OR IPS PATCH) IN:");
        ty += 64;
        vdraw_text(tx, ty, 2, RGBA8_MAXALPHA(150, 220, 255), ROM_PATH);
        ty += 28;
        vdraw_text(tx, ty, 2, RGBA8_MAXALPHA(150, 220, 255), GEN_ROM_PATH);
        ty += 28;
        vdraw_text(tx, ty, 2, RGBA8_MAXALPHA(150, 220, 255), GEN_IPS_PATH);
        ty += 42;
        vdraw_text_wrapped(tx, ty, tw, 2, RGBA8_MAXALPHA(180, 192, 208),
                           "CONFIG AND BOOT LOG ARE WRITTEN AUTOMATICALLY TO " BASE_DIR "/");
    }
    vdraw_text(tx, margin + box_h - 52, 3, RGBA8_MAXALPHA(176, 216, 255), "PRESS + TO EXIT");
}

static void draw_pause_overlay(const Pinball *pb)
{
    int vw = virt_w();
    int vh = virt_h();
    int box_w = (vw > 900) ? 760 : (vw - 64);
    int box_h = 460;
    int bx = (vw - box_w) / 2;
    int by = (vh - box_h) / 2;

    vfill_rect(0, 0, vw, vh, RGBA8(10, 14, 20, 185));
    vfill_rect(bx, by, box_w, box_h, RGBA8_MAXALPHA(24, 34, 48));
    vfill_rect(bx, by, box_w, 5, RGBA8_MAXALPHA(242, 193, 78));

    const char *orient_name = "LANDSCAPE (0 DEG)";
    if (g_config.orientation == ORIENTATION_PORTRAIT_CW) orient_name = "PORTRAIT CW (90 DEG)";
    else if (g_config.orientation == ORIENTATION_PORTRAIT_CCW) orient_name = "PORTRAIT CCW (270 DEG)";

    int tx = bx + 36;
    int ty = by + 28;
    vdraw_text(tx, ty, 3, RGBA8_MAXALPHA(255, 255, 255), "PINBALL NX PAUSED");
    ty += 44;

    char buf[128];
    snprintf(buf, sizeof buf, "GENERATION: %s", generation_full_label(pb ? pb_rom_id(pb) : g_config.generation));
    vdraw_text(tx, ty, 2, RGBA8_MAXALPHA(242, 193, 78), buf);
    ty += 28;
    snprintf(buf, sizeof buf, "ORIENTATION: %s", orient_name);
    vdraw_text(tx, ty, 2, RGBA8_MAXALPHA(210, 225, 240), buf);
    ty += 28;
    snprintf(buf, sizeof buf, "LANDSCAPE CAM: %s", g_config.landscape_follow ? "FOLLOW BALL (ZOOMED)" : "FULL TABLE");
    vdraw_text(tx, ty, 2, RGBA8_MAXALPHA(210, 225, 240), buf);
    ty += 28;
    snprintf(buf, sizeof buf, "TOUCH GUIDE: %s", g_config.show_touch_guide ? "ON" : "OFF");
    vdraw_text(tx, ty, 2, RGBA8_MAXALPHA(210, 225, 240), buf);
    ty += 42;

    vdraw_text(tx, ty, 2, RGBA8_MAXALPHA(242, 193, 78), "[+] / [A]  RESUME");
    ty += 28;
    vdraw_text(tx, ty, 2, RGBA8_MAXALPHA(176, 216, 255), "[-]        CYCLE ORIENTATION");
    ty += 28;
    vdraw_text(tx, ty, 2, RGBA8_MAXALPHA(176, 216, 255), "[L]        TOGGLE LANDSCAPE CAMERA");
    ty += 28;
    vdraw_text(tx, ty, 2, RGBA8_MAXALPHA(176, 216, 255), "[X]        TOGGLE TOUCH GUIDE");
    ty += 28;
    vdraw_text(tx, ty, 2,
               (g_rom_avail[0] && g_rom_avail[1]) ? RGBA8_MAXALPHA(150, 235, 175) : RGBA8_MAXALPHA(135, 148, 165),
               "[Y]        SWITCH GENERATION (ORIGINAL / GEN 1.6)");
    ty += 28;
    vdraw_text(tx, ty, 2, RGBA8_MAXALPHA(255, 140, 130), "[B]        EXIT TO HBMENU");

    if (g_pause_status_frames > 0 && g_pause_status[0]) {
        vdraw_text(tx, by + box_h - 36, 2, RGBA8_MAXALPHA(255, 210, 120), g_pause_status);
    }
}

static int update_landscape_camera(const Pinball *pb, int h, int vis_h)
{
    int max_y = h - vis_h;
    if (max_y <= 0) {
        g_cam.y = 0.0f;
        return 0;
    }

    bool in_game = pb_in_game(pb);
    if (g_cam.last_h != h || (!g_cam.last_in_game && in_game)) {
        g_cam.y = (float)max_y;
        g_cam.vel_y = 0.0f;
        g_cam.has_ball = false;
    }
    g_cam.last_h = h;
    g_cam.last_in_game = in_game;

    int bx = 0, by = 0;
    if (pb_ball_pos(pb, &bx, &by)) {
        if (by < 0) by = 0;
        if (by >= h) by = h - 1;

        if (g_cam.has_ball) {
            float dy = (float)by - g_cam.prev_ball_y;
            if (fabsf(dy) > 24.0f) {
                g_cam.vel_y = 0.0f;
            } else {
                g_cam.vel_y = g_cam.vel_y * 0.80f + dy * 0.20f;
            }
        } else {
            g_cam.vel_y = 0.0f;
            g_cam.has_ball = true;
        }
        g_cam.prev_ball_y = (float)by;

        float look_y = (float)by + g_cam.vel_y * 5.0f;
        if (look_y < 0.0f) look_y = 0.0f;
        if (look_y > (float)(h - 1)) look_y = (float)(h - 1);

        float target = look_y - (float)vis_h * 0.48f;
        if (target < 0.0f) target = 0.0f;
        if (target > (float)max_y) target = (float)max_y;

        float diff = target - g_cam.y;
        if (fabsf(diff) > 3.0f)
            g_cam.y += diff * 0.14f;
        else if (fabsf(diff) > 0.5f)
            g_cam.y += diff * 0.06f;
    } else {
        g_cam.has_ball = false;
        g_cam.vel_y *= 0.85f;
    }

    if (g_cam.y < 0.0f) g_cam.y = 0.0f;
    if (g_cam.y > (float)max_y) g_cam.y = (float)max_y;

    int src_y0 = (int)(g_cam.y + 0.5f);
    if (src_y0 < 0) src_y0 = 0;
    if (src_y0 > max_y) src_y0 = max_y;
    return src_y0;
}

static void draw_game(Pinball *pb, bool paused)
{
    int w = 0, h = 0, stride = 0;
    const uint32_t *src = pb_frame(pb, &w, &h, &stride);
    clear_frame(RGBA8_MAXALPHA(16, 24, 32));
    if (!src || w <= 0 || h <= 0 || stride <= 0) {
        draw_touch_guide();
        return;
    }

    if (g_config.orientation == ORIENTATION_LANDSCAPE) {
        int scale = 4;
        int src_y0 = 0;
        int vis_h = h;

        if (g_config.landscape_follow && h > 180) {
            scale = 4;
            while (scale > 1 && w * scale > SCREEN_W - 220) scale--;
            vis_h = SCREEN_H / scale;
            if (vis_h > h) vis_h = h;
            src_y0 = paused ? (int)(g_cam.y + 0.5f) : update_landscape_camera(pb, h, vis_h);
            if (src_y0 < 0) src_y0 = 0;
            if (src_y0 > h - vis_h) src_y0 = h - vis_h;
        } else {
            scale = (SCREEN_H - 16) / h;
            if (scale < 1) scale = 1;
            while (scale > 1 && w * scale > SCREEN_W - 220) scale--;
        }

        int dw = w * scale;
        int dh = vis_h * scale;
        int ox = (SCREEN_W - dw) / 2;
        int oy = (SCREEN_H - dh) / 2;

        fill_rect(0, 0, ox, SCREEN_H, RGBA8_MAXALPHA(18, 28, 40));
        fill_rect(ox + dw, 0, SCREEN_W - (ox + dw), SCREEN_H, RGBA8_MAXALPHA(18, 28, 40));

        for (int y = 0; y < vis_h; y++) {
            const uint32_t *srow = &src[(size_t)(src_y0 + y) * (size_t)stride];
            for (int sy = 0; sy < scale; sy++) {
                int dy = oy + y * scale + sy;
                if (dy < 0 || dy >= SCREEN_H) continue;
                uint32_t *drow = &g_frame[(size_t)dy * SCREEN_W + ox];
                for (int x = 0; x < w; x++) {
                    uint32_t c = gbc_to_rgba8(srow[x]);
                    for (int sx = 0; sx < scale; sx++)
                        drow[x * scale + sx] = c;
                }
            }
        }

        draw_side_hud_landscape(pb, ox);
    } else {
        int scale_x = (SCREEN_W - 32) / h;
        int scale_y = (SCREEN_H - 16) / w;
        int scale = scale_x < scale_y ? scale_x : scale_y;
        if (scale < 1) scale = 1;
        int dw = h * scale;
        int dh = w * scale;
        int ox = (SCREEN_W - dw) / 2;
        int oy = (SCREEN_H - dh) / 2;

        bool cw = (g_config.orientation == ORIENTATION_PORTRAIT_CW);
        for (int y = 0; y < h; y++) {
            const uint32_t *srow = &src[(size_t)y * (size_t)stride];
            int gx = cw ? y : (h - 1 - y);
            int dst_x0 = ox + gx * scale;
            for (int x = 0; x < w; x++) {
                uint32_t c = gbc_to_rgba8(srow[x]);
                int gy = cw ? (w - 1 - x) : x;
                int dst_y0 = oy + gy * scale;
                for (int sy = 0; sy < scale; sy++) {
                    int dy = dst_y0 + sy;
                    if (dy < 0 || dy >= SCREEN_H) continue;
                    uint32_t *drow = &g_frame[(size_t)dy * SCREEN_W + dst_x0];
                    for (int sx = 0; sx < scale; sx++)
                        drow[sx] = c;
                }
            }
        }
    }

    draw_touch_guide();
}

static void apply_touch(bool playing, uint32_t *buttons, bool *pause_tapped)
{
    static bool prev_pause_touch = false;
    HidTouchScreenState state = {0};
    size_t n = hidGetTouchScreenStates(&state, 1);

    *buttons = 0;
    bool pause_now = false;

    int vw = virt_w();
    int vh = virt_h();

    if (n > 0) {
        s32 count = state.count;
        if (count > 16) count = 16;
        for (s32 i = 0; i < count; i++) {
            int tx = (int)state.touches[i].x;
            int ty = (int)state.touches[i].y;
            int vx = tx;
            int vy = ty;

            if (g_config.orientation == ORIENTATION_PORTRAIT_CW) {
                vx = SCREEN_H - 1 - ty;
                vy = tx;
            } else if (g_config.orientation == ORIENTATION_PORTRAIT_CCW) {
                vx = ty;
                vy = SCREEN_W - 1 - tx;
            }

            if (vy < 84 && vx > vw - 156) {
                pause_now = true;
                continue;
            }

            if (playing) {
                if (vy > vh - 190) {
                    if (vx >= vw / 2 - 115 && vx <= vw / 2 + 115)
                        *buttons |= GBC_A;
                    else if (vx < vw / 2)
                        *buttons |= GBC_LEFT;
                    else
                        *buttons |= GBC_A;
                } else if (vy < vh / 3) {
                    if (vx < vw / 3) *buttons |= GBC_DOWN;
                    else if (vx > (vw * 2) / 3) *buttons |= GBC_B;
                    else *buttons |= GBC_SELECT;
                }
            } else {
                if (vy > vh - 190) {
                    if (vx >= vw / 2 - 115 && vx <= vw / 2 + 115)
                        *buttons |= GBC_START;
                    else if (vx < vw / 2)
                        *buttons |= GBC_B;
                    else
                        *buttons |= GBC_A;
                } else if (vy < vh / 3) {
                    *buttons |= GBC_UP;
                } else if (vx < vw / 3) {
                    *buttons |= GBC_LEFT;
                } else if (vx > (vw * 2) / 3) {
                    *buttons |= GBC_RIGHT;
                } else {
                    *buttons |= GBC_A;
                }
            }
        }
    }

    *pause_tapped = pause_now && !prev_pause_touch;
    prev_pause_touch = pause_now;
}

static bool video_init(void)
{
    NWindow *win = nwindowGetDefault();
    if (!win) {
        LOG_BOOT("video/framebuffer/SDL init: FAILED (nwindowGetDefault returned NULL)");
        return false;
    }

    Result fb_rc = framebufferCreate(&g_fb, win, SCREEN_W, SCREEN_H, PIXEL_FORMAT_RGBA_8888, 2);
    if (R_FAILED(fb_rc)) {
        LOG_BOOT("video/framebuffer/SDL init: FAILED (framebufferCreate rc=0x%x)", fb_rc);
        return false;
    }
    g_fb_ok = true;

    Result lin_rc = framebufferMakeLinear(&g_fb);
    if (R_FAILED(lin_rc)) {
        LOG_BOOT("video/framebuffer/SDL init: FAILED (framebufferMakeLinear rc=0x%x)", lin_rc);
        framebufferClose(&g_fb);
        g_fb_ok = false;
        return false;
    }

    g_frame_pixels = (size_t)SCREEN_W * SCREEN_H;
    g_frame = (uint32_t *)calloc(g_frame_pixels, sizeof(uint32_t));
    if (!g_frame) {
        LOG_BOOT("video/framebuffer/SDL init: FAILED (g_frame calloc failed)");
        framebufferClose(&g_fb);
        g_fb_ok = false;
        return false;
    }

    LOG_BOOT("video/framebuffer/SDL init: ok (%dx%d linear RGBA8888)", SCREEN_W, SCREEN_H);
    return true;
}

static void audio_shutdown(void)
{
    if (g_aud_started) {
        audoutStopAudioOut();
        g_aud_started = false;
    }
    if (g_aud_init_done) {
        audoutExit();
        g_aud_init_done = false;
    }
    for (int i = 0; i < AUD_NUM_BUFS; i++) {
        free(g_aud_pcm[i]);
        g_aud_pcm[i] = NULL;
        memset(&g_aud_bufs[i], 0, sizeof(g_aud_bufs[i]));
        g_aud_in_flight[i] = false;
    }
    g_aud_ok = false;
}

static void audio_init(void)
{
    Result rc = audoutInitialize();
    if (R_FAILED(rc)) {
        LOG_BOOT("audio init: FAILED (audoutInitialize rc=0x%x) - continuing muted", rc);
        return;
    }
    g_aud_init_done = true;

    u32 rate = audoutGetSampleRate();
    u32 ch = audoutGetChannelCount();
    PcmFormat fmt = audoutGetPcmFormat();
    if (rate != AUD_SAMPLE_RATE || ch != AUD_CHANNELS || fmt != PcmFormat_Int16) {
        LOG_BOOT("audio init: FAILED (unexpected format rate=%u ch=%u fmt=%d) - continuing muted",
                 rate, ch, (int)fmt);
        audio_shutdown();
        return;
    }

    for (int i = 0; i < AUD_NUM_BUFS; i++) {
        g_aud_pcm[i] = (int16_t *)memalign(0x1000, AUD_BUF_BYTES);
        if (!g_aud_pcm[i]) {
            LOG_BOOT("audio init: FAILED (memalign buf %d failed) - continuing muted", i);
            audio_shutdown();
            return;
        }
        memset(g_aud_pcm[i], 0, AUD_BUF_BYTES);
        g_aud_bufs[i].next = NULL;
        g_aud_bufs[i].buffer = g_aud_pcm[i];
        g_aud_bufs[i].buffer_size = AUD_BUF_BYTES;
        g_aud_bufs[i].data_size = AUD_BUF_BYTES;
        g_aud_bufs[i].data_offset = 0;
        g_aud_in_flight[i] = false;
    }

    rc = audoutStartAudioOut();
    if (R_FAILED(rc)) {
        LOG_BOOT("audio init: FAILED (audoutStartAudioOut rc=0x%x) - continuing muted", rc);
        audio_shutdown();
        return;
    }
    g_aud_started = true;
    g_aud_ok = true;
    LOG_BOOT("audio init: ok (%uHz stereo PcmFormat_Int16)", rate);
}

static void audio_pump(Pinball *pb)
{
    if (!g_aud_ok || !pb) return;

    for (;;) {
        AudioOutBuffer *rel = NULL;
        u32 count = 0;
        if (R_FAILED(audoutGetReleasedAudioOutBuffer(&rel, &count)) || count == 0 || !rel)
            break;
        for (int i = 0; i < AUD_NUM_BUFS; i++) {
            if (&g_aud_bufs[i] == rel) {
                g_aud_in_flight[i] = false;
                break;
            }
        }
    }

    int avail = gbc_audio_available(&pb->g);
    if (avail > 3200) pb_set_audio_rate(pb, 47800.0);
    else if (avail < 900) pb_set_audio_rate(pb, 48200.0);
    else pb_set_audio_rate(pb, (double)AUD_SAMPLE_RATE);

    for (int i = 0; i < AUD_NUM_BUFS; i++) {
        if (g_aud_in_flight[i] || !g_aud_pcm[i]) continue;
        if (gbc_audio_available(&pb->g) < 256) break;

        int got = pb_audio(pb, g_aud_pcm[i], (int)AUD_BUF_FRAMES);
        if (got <= 0) break;

        g_aud_bufs[i].data_size = (u64)got * AUD_CHANNELS * sizeof(int16_t);
        g_aud_bufs[i].data_offset = 0;
        if (R_SUCCEEDED(audoutAppendAudioOutBuffer(&g_aud_bufs[i]))) {
            g_aud_in_flight[i] = true;
        } else {
            break;
        }
    }
}

static void set_pause_status(const char *msg)
{
    snprintf(g_pause_status, sizeof(g_pause_status), "%s", msg);
    g_pause_status_frames = 180;
}

int main(void)
{
    int sw_rc = mkdir(SWITCH_DIR, 0777);
    int sw_err = (sw_rc == 0) ? 0 : errno;
    int dir_rc = mkdir(BASE_DIR, 0777);
    int dir_err = (dir_rc == 0) ? 0 : errno;

    LOG_BOOT("main enter");
    LOG_BOOT("directories created (switch=%d/errno=%d, pinball-nx=%d/errno=%d)",
             sw_rc, sw_err, dir_rc, dir_err);

    config_load_or_create();

    if (!video_init()) {
        LOG_BOOT("fatal: video init failed, exiting");
        return 1;
    }

    audio_init();

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    hidInitializeTouchScreen();
    LOG_BOOT("touch/pad init: ok");

    LOG_BOOT("ROM lookup & load: scanning %s", BASE_DIR);
    discover_roms();

    Pinball *pb = NULL;
    uint8_t *rom = NULL;
    size_t rom_size = 0;
    bool running = start_emulator_for_gen(g_config.generation, &pb, &rom, &rom_size);
    if (!running) {
        LOG_BOOT("emulator init: skipped (no valid ROM loaded)");
    }

    bool paused = false;
    u32 frame_counter = 0;
    LOG_BOOT("entering main loop (running=%d)", running ? 1 : 0);

    while (appletMainLoop()) {
        padUpdate(&g_pad);
        u64 held = padGetButtons(&g_pad);
        u64 down = padGetButtonsDown(&g_pad);

        if (!running) {
            if (down & HidNpadButton_Plus)
                break;
            draw_missing_rom();
        } else {
            uint32_t touch_buttons = 0;
            bool pause_tapped = false;
            apply_touch(!paused && pb_playing(pb), &touch_buttons, &pause_tapped);

            if (g_pause_status_frames > 0)
                g_pause_status_frames--;

            if ((down & HidNpadButton_Plus) || pause_tapped) {
                paused = !paused;
                if (paused) g_pause_status_frames = 0;
            } else if (paused) {
                if (down & HidNpadButton_A) {
                    paused = false;
                } else if (down & HidNpadButton_B) {
                    break;
                } else if (down & HidNpadButton_Minus) {
                    g_config.orientation = (Orientation)((g_config.orientation + 1) % 3);
                    config_save();
                } else if (down & HidNpadButton_X) {
                    g_config.show_touch_guide = !g_config.show_touch_guide;
                    config_save();
                } else if (down & (HidNpadButton_L | HidNpadButton_ZL)) {
                    g_config.landscape_follow = !g_config.landscape_follow;
                    camera_reset();
                    config_save();
                } else if (down & HidNpadButton_Y) {
                    int target_gen = (pb_rom_id(pb) == PB_ROM_GENERATIONS) ? PB_ROM_ORIGINAL : PB_ROM_GENERATIONS;
                    if (!g_rom_avail[target_gen]) {
                        set_pause_status(target_gen == PB_ROM_GENERATIONS
                                         ? "PLACE pokemon_pinball_generations.gbc OR .ips ON SD"
                                         : "PLACE ORIGINAL pokemon_pinball.gbc ON SD");
                    } else {
                        sram_flush(pb);
                        Pinball *new_pb = NULL;
                        uint8_t *new_rom = NULL;
                        size_t new_size = 0;
                        if (start_emulator_for_gen(target_gen, &new_pb, &new_rom, &new_size)) {
                            pb_free(pb);
                            free(pb);
                            free(rom);
                            pb = new_pb;
                            rom = new_rom;
                            rom_size = new_size;
                            set_pause_status(target_gen == PB_ROM_GENERATIONS
                                             ? "SWITCHED TO GENERATIONS 1.6"
                                             : "SWITCHED TO ORIGINAL (GEN 1)");
                        } else {
                            set_pause_status("FAILED TO SWITCH GENERATION");
                        }
                    }
                }
                draw_game(pb, true);
                draw_pause_overlay(pb);
            } else {
                if (down & HidNpadButton_Minus) {
                    g_config.orientation = (Orientation)((g_config.orientation + 1) % 3);
                    config_save();
                }

                uint32_t buttons = touch_buttons;
                if (pb_playing(pb)) {
                    if (held & (HidNpadButton_L | HidNpadButton_ZL | HidNpadButton_Left))
                        buttons |= GBC_LEFT;
                    if (held & (HidNpadButton_R | HidNpadButton_ZR | HidNpadButton_A | HidNpadButton_Down))
                        buttons |= GBC_A;
                    if (held & HidNpadButton_X)
                        buttons |= GBC_DOWN;
                    if (held & HidNpadButton_Y)
                        buttons |= GBC_B;
                    if (held & HidNpadButton_Up)
                        buttons |= GBC_SELECT;
                    if (held & HidNpadButton_B)
                        buttons |= GBC_START;
                } else {
                    if (held & HidNpadButton_A) buttons |= GBC_A;
                    if (held & HidNpadButton_B) buttons |= GBC_B;
                    if (held & HidNpadButton_X) buttons |= GBC_SELECT;
                    if (held & (HidNpadButton_Y | HidNpadButton_R | HidNpadButton_L)) buttons |= GBC_START;
                    if (held & HidNpadButton_Up) buttons |= GBC_UP;
                    if (held & HidNpadButton_Down) buttons |= GBC_DOWN;
                    if (held & HidNpadButton_Left) buttons |= GBC_LEFT;
                    if (held & HidNpadButton_Right) buttons |= GBC_RIGHT;
                }

                pb_run_frame(pb, (uint8_t)buttons);
                if (pb_fix_key_config(pb) == 1)
                    sram_flush(pb);
                audio_pump(pb);
                draw_game(pb, false);

                if (++frame_counter % 60u == 0u)
                    sram_flush(pb);
            }
        }

        if (g_fb_ok) {
            u32 stride = 0;
            u32 *fb = (u32 *)framebufferBegin(&g_fb, &stride);
            if (fb) {
                for (int y = 0; y < SCREEN_H; y++) {
                    memcpy((uint8_t *)fb + (size_t)y * stride,
                           &g_frame[(size_t)y * SCREEN_W],
                           SCREEN_W * sizeof(uint32_t));
                }
                framebufferEnd(&g_fb);
            }
        } else {
            svcSleepThread(16666666);
        }
    }

    LOG_BOOT("exiting main loop");
    audio_shutdown();
    if (pb) {
        sram_flush(pb);
        pb_free(pb);
        free(pb);
        pb = NULL;
    }
    free(rom);
    rom = NULL;
    free(g_frame);
    g_frame = NULL;
    if (g_fb_ok) {
        framebufferClose(&g_fb);
        g_fb_ok = false;
    }
    LOG_BOOT("shutdown complete");
    return 0;
}
