#include "splash.h"
#include "boot_logo.h"
#include "boot_layout.h"
#include "../drivers/display/framebuffer.h"

static int splash_on = 0;
static uint32_t splash_fill = 0;

static const uint32_t splash_ink = 0x00F2F2F5;
static const uint32_t splash_track = 0x002C2C30;

static int splash_scale(void) {
    return fb_width >= BOOT_AUTO_2X_WIDTH ? 2 : 1;
}

static uint32_t mix_black(uint32_t color, uint32_t a) {
    uint32_t r = ((color >> 16) & 0xFF) * a / 255U;
    uint32_t g = ((color >> 8) & 0xFF) * a / 255U;
    uint32_t b = (color & 0xFF) * a / 255U;
    return (r << 16) | (g << 8) | b;
}

static void draw_logo(void) {
    int s = splash_scale();
    int lw = BOOT_LOGO_W * s, lh = BOOT_LOGO_H_PX * s;
    int x0 = fb_width / 2 - lw / 2;
    int y0 = fb_height / 2 + BOOT_LOGO_CENTER_DY * s - lh / 2;
    for (int y = 0; y < lh; y++) {
        for (int x = 0; x < lw; x++) {
            uint32_t a = boot_logo_alpha[(y / s) * BOOT_LOGO_W + (x / s)];
            if (a) fb_put_pixel(x0 + x, y0 + y, mix_black(splash_ink, a));
        }
    }
}

static int inside_capsule(int sx, int sy, int w, int h) {
    int r = h * 2;
    int cy = h * 2;
    int cx;
    if (sx < r) cx = r;
    else if (sx > w * 4 - r) cx = w * 4 - r;
    else return sy >= 0 && sy < h * 4;
    return (sx - cx) * (sx - cx) + (sy - cy) * (sy - cy) <= r * r;
}

static void draw_bar(uint32_t permille) {
    int s = splash_scale();
    int w = BOOT_BAR_W * s, h = BOOT_BAR_H * s;
    int x0 = fb_width / 2 - w / 2;
    int y0 = fb_height / 2 + BOOT_BAR_TOP_DY * s;
    int fill = (int)((uint64_t)w * permille / 1000U);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t hits = 0;
            for (int sy = 0; sy < 4; sy++) {
                for (int sx = 0; sx < 4; sx++) {
                    hits += (uint32_t)inside_capsule(x * 4 + sx, y * 4 + sy, w, h);
                }
            }
            fb_put_pixel(x0 + x, y0 + y, mix_black(x < fill ? splash_ink : splash_track, hits * 255U / 16U));
        }
    }
}

static uint32_t splash_progress_permille(uint32_t stage) {
    if (stage >= 120 && stage <= 125) return 380 + (stage - 119) * 50;
    if (stage >= 1200) return 420;
    if (stage >= 13 && stage <= 21) return 680 + (stage - 13) * 35;
    if (stage >= 22) return 1000;
    if (stage <= 12) return stage * 30;
    return 0;
}

void splash_init(void) {
    if (!fb_available() || splash_on || fb_width <= 0 || fb_height <= 0) return;
    fb_clear(0x00000000);
    draw_logo();
    splash_fill = 0;
    draw_bar(0);
    splash_on = 1;
}

void splash_progress(uint32_t stage, const char *label) {
    uint32_t permille = splash_progress_permille(stage);
    (void)label;
    if (!splash_on) return;
    if (permille > 1000) permille = 1000;
    if (permille <= splash_fill) return;
    splash_fill = permille;
    draw_bar(permille);
}

void splash_finish(void) {
    if (splash_on) draw_bar(1000);
    splash_on = 0;
}

int splash_active(void) {
    return splash_on;
}
