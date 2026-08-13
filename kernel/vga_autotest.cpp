#include "vga_autotest.h"
#include "drivers/video/terminal.h"
#include "drivers/video/fb.h"
#include "serial_log.h"

int vga_autotest_run(void) {
    size_t w = terminal_get_width();
    size_t h = terminal_get_height();
    if (w < 40 || h < 10) {
        log_msg(LOG_ERR, "autotest", "vga_failed_geom");
        return -1;
    }

    size_t origin = terminal_content_origin();
    uint8_t color = 0x1E; /* yellow on blue */
    terminal_put_at(origin, 0, 'V', color);
    int cell = terminal_read_cell(origin, 0);
    if (cell < 0 || (cell & 0xFF) != 'V' || ((cell >> 8) & 0xFF) != color) {
        log_fmt3(LOG_ERR, "autotest", "vga_failed_cell", "got", (uint32_t)cell, "ok", 0u, "x", 0u);
        return -2;
    }

    terminal_status_set("vga-test");
    terminal_status_redraw();
    size_t sr = h - 1;
    int sc = terminal_read_cell(sr, 0);
    /* status_set paints a leading space then text — accept ' ' or 'v' at col 0 */
    char sch = (char)(sc & 0xFF);
    if (sc < 0 || (sch != 'v' && sch != ' ')) {
        log_msg(LOG_ERR, "autotest", "vga_failed_status");
        return -3;
    }
    if (sch == ' ') {
        int sc1 = terminal_read_cell(sr, 1);
        if (sc1 < 0 || (sc1 & 0xFF) != 'v') {
            log_msg(LOG_ERR, "autotest", "vga_failed_status");
            return -3;
        }
    }

    terminal_clear_viewport();
    int z = terminal_read_cell(origin, 0);
    if (z < 0 || (z & 0xFF) != ' ') {
        log_msg(LOG_ERR, "autotest", "vga_failed_clear");
        return -4;
    }

    log_msg(LOG_INFO, "autotest", "vga_ok");
    terminal_writestring("\n[AUTOTEST] vga ok");

    if (terminal_using_framebuffer()) {
        uint32_t fw = terminal_fb_width();
        uint32_t fh = terminal_fb_height();
        uint32_t bpp = terminal_fb_bpp();
        if (fw < 1024 || fh < 768 || bpp < 24) {
            log_fmt3(LOG_ERR, "autotest", "fb_failed", "w", fw, "h", fh, "bpp", bpp);
            return -5;
        }
        log_fmt3(LOG_INFO, "autotest", "fb_ok", "w", fw, "h", fh, "bpp", bpp);
        terminal_writestring("\n[AUTOTEST] fb ok");
    } else {
        log_msg(LOG_INFO, "autotest", "fb_skip");
        terminal_writestring("\n[AUTOTEST] fb skip");
    }
    return 0;
}
