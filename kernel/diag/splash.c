#include "splash.h"

#include "../drivers/display/font.h"
#include "../drivers/display/framebuffer.h"

static int splash_on = 0;

static const uint32_t splash_bg      = 0x00101830;   /* deep navy */
static const uint32_t splash_accent  = 0x004FC3F7;   /* light blue */
static const uint32_t splash_track   = 0x00333A48;   /* bar background */
static const uint32_t splash_muted   = 0x0098A4B8;   /* secondary text */
static const uint32_t splash_white   = 0x00FFFFFF;

static uint32_t splash_last_fill = 0;                /* bar fill permille */

/* Draw one ASCII glyph scaled by `scale` (integer pixels per font pixel),
 * centered horizontally around the caller-provided pixel origin. */
static void splash_glyph_scaled(int x0, int y0, char c, int scale,
                                uint32_t fg, uint32_t bg) {
    unsigned char uc = (unsigned char)c;
    const unsigned char *glyph;

    if (uc < FONT_FIRST || uc > FONT_LAST) uc = '?';
    glyph = font_data[uc - FONT_FIRST];

    for (int row = 0; row < FONT_HEIGHT; row++) {
        unsigned char bits = glyph[row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            uint32_t color = (bits & (0x80 >> col)) ? fg : bg;
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    fb_put_pixel(x0 + col * scale + sx, y0 + row * scale + sy, color);
                }
            }
        }
    }
}

static void splash_wordmark(const char *text, int center_x, int y, int scale) {
    int len = 0;
    while (text[len]) len++;
    int total = len * FONT_WIDTH * scale;
    int x = center_x - total / 2;
    for (int i = 0; i < len; i++) {
        splash_glyph_scaled(x, y, text[i], scale, splash_white, splash_bg);
        x += FONT_WIDTH * scale;
    }
}

/* Small centered 1x line (normal font size). */
static void splash_text_center(const char *text, int center_x, int y,
                               uint32_t color) {
    int len = 0;
    while (text[len]) len++;
    int total = len * FONT_CELL_WIDTH;
    int x = center_x - total / 2;
    for (int i = 0; i < len; i++) {
        fb_draw_char(x, y, text[i], color, splash_bg);
        x += FONT_CELL_WIDTH;
    }
}

static uint32_t splash_progress_permille(uint32_t stage) {
    if (stage >= 120 && stage <= 125) {
        /* network / hda / audio / nvme / ahci / ata live in the S12 gap */
        return 380 + (stage - 119) * 50;
    }
    if (stage >= 13 && stage <= 21) {
        return 680 + (stage - 13) * 29;
    }
    if (stage >= 22) {
        return 1000;
    }
    if (stage <= 12) {
        return stage * 30;
    }
    return 0;
}

void splash_init(void) {
    int w;
    int h;
    int bar_w;
    int bar_x;
    int bar_y;

    if (!fb_available() || splash_on) return;
    w = fb_width;
    h = fb_height;
    if (w <= 0 || h <= 0) return;

    fb_clear(splash_bg);

    /* Wordmark + tagline, vertically balanced around the upper third. */
    splash_wordmark("ICDA", w / 2, h / 2 - 190, 8);
    splash_text_center("operating system", w / 2, h / 2 - 40, splash_muted);

    /* Progress bar track. */
    bar_w = w / 2;
    if (bar_w > 640) bar_w = 640;
    bar_x = (w - bar_w) / 2;
    bar_y = h / 2 + 60;
    fb_fill_rect(bar_x, bar_y, bar_w, 6, splash_track);
    fb_fill_rect(bar_x, bar_y + 6, bar_w, 1, 0x00202A40);

    splash_text_center("starting", w / 2, bar_y + 22, splash_muted);

    splash_last_fill = 0;
    splash_on = 1;
    splash_progress(1, "serial");
}

void splash_progress(uint32_t stage, const char *label) {
    int w;
    int bar_w;
    int bar_x;
    int bar_y;
    uint32_t permille = splash_progress_permille(stage);
    uint32_t fill;

    if (!splash_on) return;
    w = fb_width;
    if (w <= 0) return;
    bar_w = w / 2;
    if (bar_w > 640) bar_w = 640;
    bar_x = (w - bar_w) / 2;
    bar_y = fb_height / 2 + 60;

    if (permille > 1000) permille = 1000;
    if (permille < splash_last_fill) permille = splash_last_fill;
    fill = (uint32_t)((uint64_t)bar_w * permille / 1000);
    if (fill > splash_last_fill) {
        fb_fill_rect(bar_x + (int)splash_last_fill, bar_y,
                     (int)(fill - splash_last_fill), 6, splash_accent);
        splash_last_fill = fill;
    }

    /* Stage label under the bar - the one piece of boot telemetry kept
     * on screen, so a hang still names the failing subsystem. */
    if (label && *label) {
        splash_text_center(label, w / 2, bar_y + 40, splash_muted);
    }
}

void splash_finish(void) {
    splash_on = 0;
}

int splash_active(void) {
    return splash_on;
}
