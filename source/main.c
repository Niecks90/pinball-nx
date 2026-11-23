#include <switch.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/core/gbc.h"
#include "../src/game/pinball.h"

#define SCREEN_W 1280
#define SCREEN_H 720
#define ROM_PATH "sdmc:/switch/pinball-nx/pokemon_pinball.gbc"
#define CFG_PATH "sdmc:/switch/pinball-nx/config.ini"

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
static Config g_config = { ORIENTATION_LANDSCAPE, true };
static uint32_t *g_frame;
static size_t g_frame_pixels;

static void config_load(void)
{
    FILE *f = fopen(CFG_PATH, "rb");
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof line, f)) {
        int v;
        if (sscanf(line, "orientation=%d", &v) == 1 && v >= 0 && v <= 2)
            g_config.orientation = (Orientation)v;
        if (sscanf(line, "touch_guide=%d", &v) == 1)
            g_config.show_touch_guide = v != 0;
    }
    fclose(f);
}

static void config_save(void)
{
    FILE *f = fopen(CFG_PATH, "wb");
    if (!f) return;
    fprintf(f, "orientation=%d\ntouch_guide=%d\n", (int)g_config.orientation,
            g_config.show_touch_guide ? 1 : 0);
    fclose(f);
}

static uint8_t *load_rom(size_t *size)
{
    FILE *f = fopen(ROM_PATH, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n <= 0 || n > 16 * 1024 * 1024) { fclose(f); return NULL; }
    rewind(f);
    uint8_t *rom = malloc((size_t)n);
    if (!rom || fread(rom, 1, (size_t)n, f) != (size_t)n) {
        free(rom);
        rom = NULL;
    }
    fclose(f);
    if (rom) *size = (size_t)n;
    return rom;
}

static void clear_frame(uint32_t color)
{
    for (size_t i = 0; i < g_frame_pixels; i++) g_frame[i] = color;
}

static void rect(int x, int y, int w, int h, uint32_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCREEN_W) w = SCREEN_W - x;
    if (y + h > SCREEN_H) h = SCREEN_H - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            g_frame[(size_t)yy * SCREEN_W + xx] = color;
}

static void draw_text_stub(int x, int y, const char *text, uint32_t color)
{
    /* A tiny readable block font keeps the error/status path independent of assets. */
    for (size_t i = 0; text[i]; i++) {
        if (text[i] != ' ') rect(x + (int)i * 8, y, 5, 10, color);
    }
}

static void draw_touch_guide(void)
{
    if (!g_config.show_touch_guide) return;
    rect(0, SCREEN_H - 130, 320, 130, 0x55305090);
    rect(SCREEN_W - 320, SCREEN_H - 130, 320, 130, 0x55305090);
    rect(SCREEN_W / 2 - 90, SCREEN_H - 100, 180, 90, 0x55408040);
    draw_text_stub(24, SCREEN_H - 100, "LEFT", 0xFFFFFFFF);
    draw_text_stub(SCREEN_W - 290, SCREEN_H - 100, "RIGHT", 0xFFFFFFFF);
    draw_text_stub(SCREEN_W / 2 - 60, SCREEN_H - 70, "PLUNGER", 0xFFFFFFFF);
}

static void draw_missing_rom(void)
{
    clear_frame(0xFF101820);
    rect(140, 180, SCREEN_W - 280, 300, 0xFF203040);
    draw_text_stub(220, 240, "PINBALL NX", 0xFFFFFFFF);
    draw_text_stub(220, 290, "ROM REQUIRED", 0xFFFFCC66);
    draw_text_stub(220, 350, "COPY POKEMON_PINBALL.GBC", 0xFFFFFFFF);
    draw_text_stub(220, 390, "TO SDMC SWITCH PINBALL-NX", 0xFFFFFFFF);
    draw_text_stub(220, 440, "PRESS + FOR HELP", 0xFFB0D8FF);
}

static void draw_game(const Pinball *pb)
{
    int w, h, stride;
    const uint32_t *src = pb_frame((Pinball *)pb, &w, &h, &stride);
    clear_frame(0xFF101820);
    int scale = (SCREEN_H - 40) / (h > 0 ? h : 1);
    if (scale < 1) scale = 1;
    int dw = w * scale;
    int dh = h * scale;
    if (dw > SCREEN_W - 260) {
        scale = (SCREEN_W - 260) / (w > 0 ? w : 1);
        if (scale < 1) scale = 1;
        dw = w * scale;
        dh = h * scale;
    }
    int ox = (SCREEN_W - 260 - dw) / 2 + 130;
    int oy = (SCREEN_H - dh) / 2;
    rect(0, 0, 130, SCREEN_H, 0xFF182A3A);
    rect(SCREEN_W - 130, 0, 130, SCREEN_H, 0xFF182A3A);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t c = src[y * stride + x] | 0xFF000000;
            rect(ox + x * scale, oy + y * scale, scale, scale, c);
        }
    draw_touch_guide();
}

static void apply_touch(uint32_t *buttons)
{
    HidTouchScreenState state;
    hidInitializeTouchScreen();
    int count = hidGetTouchScreenStates(&state, 1);
    *buttons = 0;
    for (int i = 0; i < count; i++) {
        int x = state.touches[i].x;
        int y = state.touches[i].y;
        if (g_config.orientation == ORIENTATION_PORTRAIT_CW) {
            int t = x; x = SCREEN_H - y; y = t;
        } else if (g_config.orientation == ORIENTATION_PORTRAIT_CCW) {
            int t = x; x = y; y = SCREEN_W - t;
        }
        if (y > SCREEN_H - 160 && x < SCREEN_W / 2) *buttons |= GBC_B;
        else if (y > SCREEN_H - 160) *buttons |= GBC_A;
        else if (x < SCREEN_W / 3) *buttons |= GBC_LEFT;
        else if (x > SCREEN_W * 2 / 3) *buttons |= GBC_RIGHT;
        else if (y < SCREEN_H / 3) *buttons |= GBC_UP;
    }
}

int main(void)
{
    consoleInit(NULL);
    framebufferCreate(&g_fb, nwindowGetDefault(), SCREEN_W, SCREEN_H, PIXEL_FORMAT_RGBA_8888, 2);
    fsdevMountSdmc();
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    config_load();

    g_frame_pixels = (size_t)SCREEN_W * SCREEN_H;
    g_frame = calloc(g_frame_pixels, sizeof(uint32_t));
    size_t rom_size = 0;
    uint8_t *rom = load_rom(&rom_size);
    Pinball pb;
    bool running = rom && pb_init(&pb, rom, rom_size, 48000) == 0;
    if (running) {
        pb_set_narrow(&pb, true);
        pb_set_antilag(&pb, true);
        pb_set_double_speed(&pb, true);
        pb_set_frame_palettes(&pb, true);
        pb_set_hold(&pb, true);
    }

    while (appletMainLoop()) {
        padUpdate(&g_pad);
        u64 held = padGetButtons(&g_pad);
        u64 down = padGetButtonsDown(&g_pad);
        if (down & HidNpadButton_Minus) {
            g_config.orientation = (Orientation)((g_config.orientation + 1) % 3);
            config_save();
        }
        if (down & HidNpadButton_X) g_config.show_touch_guide = !g_config.show_touch_guide;
        if (down & HidNpadButton_Plus) {
            consoleClear();
            printf("Pinball NX\n\nOrientation: %d\nTouch guide: %s\nPress - to rotate, X to toggle guide.\n",
                   (int)g_config.orientation, g_config.show_touch_guide ? "on" : "off");
        }

        uint32_t buttons = 0;
        apply_touch(&buttons);
        if (held & (HidNpadButton_L | HidNpadButton_ZL | HidNpadButton_Left)) buttons |= GBC_B;
        if (held & (HidNpadButton_R | HidNpadButton_ZR | HidNpadButton_A)) buttons |= GBC_A;
        if (held & HidNpadButton_Down) buttons |= GBC_DOWN;
        if (held & HidNpadButton_X) buttons |= GBC_LEFT;
        if (held & HidNpadButton_Y) buttons |= GBC_RIGHT;

        if (running) {
            pb_run_frame(&pb, (uint8_t)buttons);
            draw_game(&pb);
        } else {
            draw_missing_rom();
        }

        u32 stride;
        u32 *fb = (u32 *)framebufferBegin(&g_fb, &stride);
        if (fb) {
            for (int y = 0; y < SCREEN_H; y++)
                memcpy((uint8_t *)fb + y * stride, &g_frame[(size_t)y * SCREEN_W], SCREEN_W * sizeof(uint32_t));
            framebufferEnd(&g_fb);
        }
        svcSleepThread(16666666);
    }

    if (running) pb_free(&pb);
    free(rom);
    free(g_frame);
    fsdevUnmountDevice("sdmc");
    framebufferClose(&g_fb);
    consoleExit(NULL);
    return 0;
}
