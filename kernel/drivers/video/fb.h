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
    uint8_t scale; /* integer glyph zoom; usually 1, up only if needed */
    uint32_t origin_x; /* pixel offset of cell (0,0) */
    uint32_t origin_y;
    bool active;
};

/* Parse Multiboot2 info; returns true if usable RGB framebuffer found. */
bool fb_init_from_multiboot(uint32_t mbi_addr);

const struct fb_info* fb_get_info(void);
bool fb_active(void);
uint8_t fb_scale(void);
uint32_t fb_cell_width(void);
uint32_t fb_cell_height(void);
/* Place the text grid inside the FB (centers + keeps bottom status on-screen). */
void fb_set_console_origin(uint32_t origin_x, uint32_t origin_y);

void fb_clear(uint32_t rgb);
void fb_draw_glyph(size_t cell_x, size_t cell_y, char c, uint8_t vga_color);
void fb_fill_cell(size_t cell_x, size_t cell_y, uint8_t vga_color);
/* Scroll a rectangular cell band up by `lines` cell-rows using memmove (fast). */
void fb_scroll_cells_up(size_t cell_row0, size_t cell_rows, size_t cell_cols,
                        size_t lines, uint8_t fill_vga);

#define FB_FONT_W 8
#define FB_FONT_H 16

#endif
