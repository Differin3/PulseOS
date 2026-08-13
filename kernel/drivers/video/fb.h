#ifndef DRIVERS_VIDEO_FB_H
#define DRIVERS_VIDEO_FB_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct fb_info {
    uint32_t addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t bpp;
    bool active;
};

/* Parse Multiboot2 info; returns true if usable RGB framebuffer found. */
bool fb_init_from_multiboot(uint32_t mbi_addr);

const struct fb_info* fb_get_info(void);
bool fb_active(void);

void fb_clear(uint32_t rgb);
void fb_draw_glyph(size_t cell_x, size_t cell_y, char c, uint8_t vga_color);
void fb_fill_cell(size_t cell_x, size_t cell_y, uint8_t vga_color);

#define FB_FONT_W 8
#define FB_FONT_H 16

#endif
