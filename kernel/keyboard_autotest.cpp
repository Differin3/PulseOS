#include "keyboard_autotest.h"
#include "drivers/input/keyboard.h"
#include "drivers/video/terminal.h"
#include "serial_log.h"

static void kbd_ok(const char* step) {
    terminal_writestring("\n[AUTOTEST] kbd ");
    terminal_writestring(step);
    log_msg(LOG_INFO, "autotest", step);
}

static void kbd_fail(const char* step) {
    terminal_writestring("\n[AUTOTEST] kbd FAIL ");
    terminal_writestring(step);
    log_msg(LOG_ERR, "autotest", step);
}

static void inject_bytes(const uint8_t* bytes, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        keyboard_inject_scancode(bytes[i]);
    }
}

static int expect_seq(const char* expect, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        char c = keyboard_getchar();
        if (c != expect[i]) return -1;
    }
    if (keyboard_getchar() != 0) return -2;
    return 0;
}

static void tap(uint8_t make) {
    keyboard_inject_scancode(make);
    keyboard_inject_scancode((uint8_t)(make | 0x80));
}

static int kbd_fail_end(const char* step, int code) {
    kbd_fail(step);
    keyboard_test_end();
    return code;
}

int keyboard_autotest_run(void) {
    terminal_writestring("\n[AUTOTEST] keyboard start");
    log_msg(LOG_INFO, "autotest", "keyboard_start");

    /* --- ascii: hello --- */
    keyboard_test_reset();
    {
        const uint8_t seq[] = {
            0x23, 0xA3, /* h */
            0x12, 0x92, /* e */
            0x26, 0xA6, /* l */
            0x26, 0xA6, /* l */
            0x18, 0x98  /* o */
        };
        inject_bytes(seq, sizeof(seq));
        if (expect_seq("hello", 5) != 0) return kbd_fail_end("kbd_ascii_fail", -1);
    }
    kbd_ok("kbd_ascii_ok");

    /* --- shift: ! and A --- */
    keyboard_test_reset();
    {
        const uint8_t seq[] = {
            0x2A,       /* LShift make */
            0x02, 0x82, /* 1 -> ! */
            0x1E, 0x9E, /* a -> A */
            0xAA        /* LShift break */
        };
        inject_bytes(seq, sizeof(seq));
        if (expect_seq("!A", 2) != 0) return kbd_fail_end("kbd_shift_fail", -2);
    }
    kbd_ok("kbd_shift_ok");

    /* --- edit: Backspace Tab Enter --- */
    keyboard_test_reset();
    {
        const uint8_t seq[] = {
            0x0E, 0x8E, /* Backspace */
            0x0F, 0x8F, /* Tab */
            0x1C, 0x9C  /* Enter */
        };
        inject_bytes(seq, sizeof(seq));
        char e[3] = { '\b', '\t', '\n' };
        if (expect_seq(e, 3) != 0) return kbd_fail_end("kbd_edit_fail", -3);
    }
    kbd_ok("kbd_edit_ok");

    /* --- specials: Up / Home / End must be KEY_* not 8/9 --- */
    keyboard_test_reset();
    {
        const uint8_t seq[] = {
            0xE0, 0x48, 0xE0, 0xC8, /* Up */
            0xE0, 0x47, 0xE0, 0xC7, /* Home */
            0xE0, 0x4F, 0xE0, 0xCF  /* End */
        };
        inject_bytes(seq, sizeof(seq));
        char c0 = keyboard_getchar();
        char c1 = keyboard_getchar();
        char c2 = keyboard_getchar();
        if ((uint8_t)c0 != KEY_UP || (uint8_t)c1 != KEY_HOME || (uint8_t)c2 != KEY_END)
            return kbd_fail_end("kbd_special_fail", -4);
        if (keyboard_getchar() != 0) return kbd_fail_end("kbd_special_extra", -4);
    }
    kbd_ok("kbd_special_ok");

    /* --- ctrl: Ctrl+C -> 3, Ctrl+L -> 12 --- */
    keyboard_test_reset();
    {
        const uint8_t seq[] = {
            0x1D,       /* LCtrl make */
            0x2E, 0xAE, /* c */
            0x26, 0xA6, /* l */
            0x9D        /* LCtrl break */
        };
        inject_bytes(seq, sizeof(seq));
        char e[2] = { 3, 12 };
        if (expect_seq(e, 2) != 0) return kbd_fail_end("kbd_ctrl_fail", -5);
    }
    kbd_ok("kbd_ctrl_ok");

    /* --- caps: Caps then a -> A --- */
    keyboard_test_reset();
    {
        tap(0x3A); /* Caps toggle on */
        tap(0x1E); /* a */
        if (expect_seq("A", 1) != 0) return kbd_fail_end("kbd_caps_fail", -6);
        if (!keyboard_caps_on()) return kbd_fail_end("kbd_caps_state", -6);
    }
    kbd_ok("kbd_caps_ok");

    /* --- burst: 100 keys without drops --- */
    keyboard_test_reset();
    {
        for (int i = 0; i < 100; i++) tap(0x1E);
        if (keyboard_drop_count() != 0) return kbd_fail_end("kbd_burst_drop", -7);
        for (int i = 0; i < 100; i++) {
            if (keyboard_getchar() != 'a') return kbd_fail_end("kbd_burst_data", -7);
        }
        if (keyboard_getchar() != 0) return kbd_fail_end("kbd_burst_extra", -7);
    }
    kbd_ok("kbd_burst_ok");

    keyboard_test_end();
    terminal_writestring("\n[AUTOTEST] keyboard ok");
    log_msg(LOG_INFO, "autotest", "keyboard_ok");
    return 0;
}
