#include <switch.h>
#include <errno.h>
#include <malloc.h>
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
#define CFG_PATH        BASE_DIR "/config.ini"
#define LOG_PATH        BASE_DIR "/boot.log"

#define AUD_SAMPLE_RATE 48000u
#define AUD_CHANNELS    2u
#define AUD_BUF_FRAMES  1024u
#define AUD_BUF_BYTES   (AUD_BUF_FRAMES * AUD_CHANNELS * sizeof(int16_t))
#define AUD_NUM_BUFS    4

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
} Config;

static PadState g_pad;
static Framebuffer g_fb;
static bool g_fb_ok = false;

static Config g_config = { ORIENTATION_LANDSCAPE, true };
static uint32_t *g_frame = NULL;
static size_t g_frame_pixels = 0;

static AudioOutBuffer g_aud_bufs[AUD_NUM_BUFS];
static int16_t *g_aud_pcm[AUD_NUM_BUFS];
static bool g_aud_in_flight[AUD_NUM_BUFS];
static bool g_aud_started = false;
static bool g_aud_init_done = false;
static bool g_aud_ok = false;

static bool g_boot_log_started = false;
static char g_rom_error[256] = "";

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
    fprintf(f, "orientation=%d\ntouch_guide=%d\n",
            (int)g_config.orientation,
            g_config.show_touch_guide ? 1 : 0);
    fflush(f);
    fclose(f);
    return true;
}

static void config_load_or_create(void)
{
    FILE *f = fopen(CFG_PATH, "rb");
    if (!f) {
        bool created = config_save();
        LOG_BOOT("config.ini created (%s): orientation=%d touch_guide=%d",
                 created ? "ok" : "write_failed",
                 (int)g_config.orientation,
                 g_config.show_touch_guide ? 1 : 0);
        return;
    }

    char line[64];
    while (fgets(line, sizeof line, f)) {
        int v = 0;
        if (sscanf(line, "orientation=%d", &v) == 1 && v >= 0 && v <= 2)
            g_config.orientation = (Orientation)v;
        if (sscanf(line, "touch_guide=%d", &v) == 1)
            g_config.show_touch_guide = (v != 0);
    }
    fclose(f);
    LOG_BOOT("config.ini loaded: orientation=%d touch_guide=%d",
             (int)g_config.orientation,
             g_config.show_touch_guide ? 1 : 0);
}

static uint8_t *load_rom(size_t *size)
{
    *size = 0;
    FILE *f = fopen(ROM_PATH, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n <= 0 || n > 16L * 1024L * 1024L) {
        fclose(f);
        return NULL;
    }
    rewind(f);

    uint8_t *rom = (uint8_t *)malloc((size_t)n);
    if (!rom) {
        fclose(f);
        return NULL;
    }
    if (fread(rom, 1, (size_t)n, f) != (size_t)n) {
        free(rom);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return rom;
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

static void draw_text(int x, int y, int scale, uint32_t color, const char *text)
{
    if (!g_frame || !text || scale < 1) return;
    font_init();
    int cx = x;
    const char *p = text;
    while (*p) {
        if (*p == '\n') {
            cx = x;
            y += FONT_CELL_H * scale;
            p++;
            continue;
        }
        uint32_t cp = utf8_next(&p);
        const FontGlyph *g = font_glyph(cp);
        for (int r = 0; r < FONT_H; r++) {
            uint8_t bits = g->rows[r];
            for (int c = 0; c < FONT_W; c++) {
                if (bits & (0x10 >> c))
                    fill_rect(cx + c * scale, y + r * scale, scale, scale, color);
            }
        }
        cx += FONT_CELL_W * scale;
    }
}

static void draw_text_wrapped(int x, int y, int max_w, int scale, uint32_t color, const char *text)
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
        draw_text(x, y, scale, color, line);
        y += FONT_CELL_H * scale + 4;
        p += len;
        while (*p == ' ') p++;
        if (*p == '\n') p++;
    }
}

static void draw_touch_guide(void)
{
    if (!g_config.show_touch_guide) return;

    if (g_config.orientation == ORIENTATION_LANDSCAPE) {
        fill_rect(0, SCREEN_H - 150, 260, 150, RGBA8(48, 80, 144, 96));
        fill_rect(SCREEN_W - 260, SCREEN_H - 150, 260, 150, RGBA8(48, 80, 144, 96));
        fill_rect(SCREEN_W / 2 - 110, SCREEN_H - 96, 220, 96, RGBA8(64, 128, 64, 96));
        fill_rect(SCREEN_W - 140, 0, 140, 80, RGBA8(90, 90, 110, 96));
        draw_text(28, SCREEN_H - 92, 3, RGBA8_MAXALPHA(255, 255, 255), "L FLIPPER");
        draw_text(SCREEN_W - 232, SCREEN_H - 92, 3, RGBA8_MAXALPHA(255, 255, 255), "R FLIPPER");
        draw_text(SCREEN_W / 2 - 63, SCREEN_H - 60, 3, RGBA8_MAXALPHA(255, 255, 255), "PLUNGER");
        draw_text(SCREEN_W - 116, 28, 2, RGBA8_MAXALPHA(230, 230, 230), "PAUSE");
    } else if (g_config.orientation == ORIENTATION_PORTRAIT_CW) {
        fill_rect(SCREEN_W - 160, SCREEN_H / 2, 160, SCREEN_H / 2, RGBA8(48, 80, 144, 96));
        fill_rect(SCREEN_W - 160, 0, 160, SCREEN_H / 2, RGBA8(48, 80, 144, 96));
        draw_text(SCREEN_W - 144, SCREEN_H - 120, 2, RGBA8_MAXALPHA(255, 255, 255), "L FLIP");
        draw_text(SCREEN_W - 144, 48, 2, RGBA8_MAXALPHA(255, 255, 255), "R FLIP");
    } else {
        fill_rect(0, 0, 160, SCREEN_H / 2, RGBA8(48, 80, 144, 96));
        fill_rect(0, SCREEN_H / 2, 160, SCREEN_H / 2, RGBA8(48, 80, 144, 96));
        draw_text(16, 48, 2, RGBA8_MAXALPHA(255, 255, 255), "L FLIP");
        draw_text(16, SCREEN_H - 120, 2, RGBA8_MAXALPHA(255, 255, 255), "R FLIP");
    }
}

static void draw_missing_rom(void)
{
    clear_frame(RGBA8_MAXALPHA(14, 18, 24));
    fill_rect(100, 110, SCREEN_W - 200, SCREEN_H - 220, RGBA8_MAXALPHA(26, 36, 50));
    fill_rect(100, 110, SCREEN_W - 200, 6, RGBA8_MAXALPHA(242, 193, 78));

    draw_text(150, 150, 5, RGBA8_MAXALPHA(255, 255, 255), "PINBALL NX");
    if (g_rom_error[0]) {
        draw_text(150, 220, 3, RGBA8_MAXALPHA(255, 130, 110), "ROM COULD NOT BE LOADED");
        draw_text_wrapped(150, 275, SCREEN_W - 300, 2, RGBA8_MAXALPHA(230, 236, 244), g_rom_error);
    } else {
        draw_text(150, 220, 3, RGBA8_MAXALPHA(242, 193, 78), "POKEMON PINBALL ROM REQUIRED");
        draw_text(150, 285, 2, RGBA8_MAXALPHA(220, 228, 238), "COPY YOUR COMPATIBLE US ROM TO:");
        draw_text(150, 325, 3, RGBA8_MAXALPHA(150, 220, 255), ROM_PATH);
        draw_text(150, 385, 2, RGBA8_MAXALPHA(180, 192, 208),
                  "CONFIG AND BOOT LOG ARE WRITTEN TO " BASE_DIR "/");
    }
    draw_text(150, SCREEN_H - 175, 3, RGBA8_MAXALPHA(176, 216, 255), "PRESS + TO EXIT");
}

static void draw_pause_overlay(void)
{
    fill_rect(0, 0, SCREEN_W, SCREEN_H, RGBA8(10, 14, 20, 185));
    fill_rect(220, 170, SCREEN_W - 440, 380, RGBA8_MAXALPHA(24, 34, 48));
    fill_rect(220, 170, SCREEN_W - 440, 5, RGBA8_MAXALPHA(242, 193, 78));

    const char *orient_name = "LANDSCAPE (0 DEG)";
    if (g_config.orientation == ORIENTATION_PORTRAIT_CW) orient_name = "PORTRAIT CW (90 DEG)";
    else if (g_config.orientation == ORIENTATION_PORTRAIT_CCW) orient_name = "PORTRAIT CCW (270 DEG)";

    draw_text(270, 210, 4, RGBA8_MAXALPHA(255, 255, 255), "PINBALL NX PAUSED");
    char buf[96];
    snprintf(buf, sizeof buf, "ORIENTATION: %s", orient_name);
    draw_text(270, 285, 2, RGBA8_MAXALPHA(210, 225, 240), buf);
    snprintf(buf, sizeof buf, "TOUCH GUIDE: %s", g_config.show_touch_guide ? "ON" : "OFF");
    draw_text(270, 320, 2, RGBA8_MAXALPHA(210, 225, 240), buf);

    draw_text(270, 385, 2, RGBA8_MAXALPHA(242, 193, 78), "[+] / [A]  RESUME");
    draw_text(270, 415, 2, RGBA8_MAXALPHA(176, 216, 255), "[-]        CYCLE ORIENTATION");
    draw_text(270, 445, 2, RGBA8_MAXALPHA(176, 216, 255), "[X]        TOGGLE TOUCH GUIDE");
    draw_text(270, 475, 2, RGBA8_MAXALPHA(255, 140, 130), "[B]        EXIT TO HBMENU");
}

static void draw_game(Pinball *pb)
{
    int w = 0, h = 0, stride = 0;
    const uint32_t *src = pb_frame(pb, &w, &h, &stride);
    clear_frame(RGBA8_MAXALPHA(16, 24, 32));
    if (!src || w <= 0 || h <= 0 || stride <= 0) {
        draw_touch_guide();
        return;
    }

    if (g_config.orientation == ORIENTATION_LANDSCAPE) {
        int scale = (SCREEN_H - 16) / h;
        if (scale < 1) scale = 1;
        while (scale > 1 && w * scale > SCREEN_W - 260) scale--;
        int dw = w * scale;
        int dh = h * scale;
        int ox = (SCREEN_W - dw) / 2;
        int oy = (SCREEN_H - dh) / 2;

        fill_rect(0, 0, ox, SCREEN_H, RGBA8_MAXALPHA(20, 32, 44));
        fill_rect(ox + dw, 0, SCREEN_W - (ox + dw), SCREEN_H, RGBA8_MAXALPHA(20, 32, 44));

        for (int y = 0; y < h; y++) {
            const uint32_t *srow = &src[(size_t)y * (size_t)stride];
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

    int vw = (g_config.orientation == ORIENTATION_LANDSCAPE) ? SCREEN_W : SCREEN_H;
    int vh = (g_config.orientation == ORIENTATION_LANDSCAPE) ? SCREEN_H : SCREEN_W;

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

            if (vy < 90 && vx > vw - 150) {
                pause_now = true;
                continue;
            }

            if (playing) {
                if (vy > vh - 200) {
                    if (vx >= vw / 2 - 120 && vx <= vw / 2 + 120)
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
                if (vy > vh - 200) {
                    if (vx >= vw / 2 - 120 && vx <= vw / 2 + 120)
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

    LOG_BOOT("ROM lookup & load: checking %s", ROM_PATH);
    size_t rom_size = 0;
    uint8_t *rom = load_rom(&rom_size);

    Pinball *pb = NULL;
    bool running = false;
    if (!rom) {
        LOG_BOOT("ROM lookup & load: not found or unreadable");
        LOG_BOOT("emulator init: skipped (missing ROM)");
    } else {
        LOG_BOOT("ROM lookup & load: loaded %zu bytes", rom_size);
        pb = (Pinball *)calloc(1, sizeof(Pinball));
        if (!pb) {
            snprintf(g_rom_error, sizeof g_rom_error, "Out of memory allocating emulator state.");
            LOG_BOOT("emulator init: FAILED (calloc Pinball %zu bytes failed)", sizeof(Pinball));
            free(rom);
            rom = NULL;
        } else if (pb_init(pb, rom, rom_size, AUD_SAMPLE_RATE) != 0) {
            snprintf(g_rom_error, sizeof g_rom_error, "%s",
                     pb->error[0] ? pb->error : "Unsupported or invalid ROM.");
            LOG_BOOT("emulator init: FAILED (%s)", g_rom_error);
            pb_free(pb);
            free(pb);
            pb = NULL;
            free(rom);
            rom = NULL;
        } else {
            pb_set_narrow(pb, true);
            pb_set_antilag(pb, true);
            pb_set_double_speed(pb, true);
            pb_set_frame_palettes(pb, true);
            pb_set_hold(pb, true);
            pb_set_audio_highpass(pb, GBC_HIGHPASS_FRONT);
            running = true;
            LOG_BOOT("emulator init: ok");
        }
    }

    bool paused = false;
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

            if ((down & HidNpadButton_Plus) || pause_tapped) {
                paused = !paused;
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
                }
                draw_game(pb);
                draw_pause_overlay();
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
                audio_pump(pb);
                draw_game(pb);
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
