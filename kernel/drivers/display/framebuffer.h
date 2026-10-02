#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <stdint.h>


#define FB_BLACK        0x00000000
#define FB_WHITE        0x00FFFFFF
#define FB_RED          0x00FF0000
#define FB_GREEN        0x0000FF00
#define FB_BLUE         0x000000FF
#define FB_CYAN         0x0000FFFF
#define FB_YELLOW       0x00FFFF00
#define FB_MAGENTA      0x00FF00FF
#define FB_GRAY         0x00888888
#define FB_DARK_GRAY    0x00444444
#define FB_LIGHT_GRAY   0x00CCCCCC
#define FB_ORANGE       0x00FF8800



int  fb_init(void* multiboot_info);

uint64_t fb_phys_addr(void);

uint64_t fb_phys_size(void);

int fb_bpp_value(void);
uint32_t fb_pitch_value(void);

void fb_remap(uint64_t physical_base);
void fb_clear(uint32_t color);


void fb_set_double_frame(int enable);
void fb_put_pixel(int x, int y, uint32_t color);
void fb_fill_rect(int x, int y, int w, int h, uint32_t color);
void fb_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg);
void fb_print(const char* str, uint32_t fg, uint32_t bg);
void fb_print_at(int x, int y, const char* str, uint32_t fg, uint32_t bg);
void fb_write_at_cells(int col, int row, const char* str, uint32_t fg, uint32_t bg);
int fb_columns(void);
int fb_rows(void);
void fb_print_int(int n, uint32_t fg, uint32_t bg);
void fb_print_hex(unsigned int n, uint32_t fg, uint32_t bg);
void fb_newline();
void fb_backspace(uint32_t bg);
void fb_set_cursor(int x, int y);
void fb_get_cursor(int *x_out, int *y_out);
int  fb_available();



void fb_adopt(uint64_t phys, uint32_t pitch, int width, int height, int bpp);


extern int fb_width;
extern int fb_height;

#endif
