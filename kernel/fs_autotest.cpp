#include "fs_autotest.h"
#include "fs.h"
#include "serial_log.h"
#include "drivers/video/terminal.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

static void fs_at_ok(const char* step) {
    terminal_writestring("\n[AUTOTEST] fs ");
    terminal_writestring(step);
    terminal_writestring(" ok");
    log_msg(LOG_INFO, "autotest", step);
}

static void fs_at_fail(const char* step) {
    terminal_writestring("\n[AUTOTEST] fs ");
    terminal_writestring(step);
    terminal_writestring(" FAIL");
    log_msg(LOG_ERR, "autotest", step);
}

static bool fs_at_contains(const char* hay, const char* needle) {
    if (!hay || !needle || !needle[0]) return false;
    for (size_t i = 0; hay[i]; i++) {
        size_t j = 0;
        while (needle[j] && hay[i + j] == needle[j]) j++;
        if (!needle[j]) return true;
    }
    return false;
}

static int fs_at_expect_write_read(const char* path, const char* data, size_t len) {
    if (fs_write(path, data, len) != 0) {
        return -1;
    }
    uint32_t size = 0;
    if (fs_open(path, &size) != 0) {
        return -1;
    }
    if ((size_t)size != len) {
        return -1;
    }
    char buf[1024];
    if (len > sizeof(buf)) return -1;
    if (fs_read(path, buf, len) < 0) {
        return -1;
    }
    if (memcmp(buf, data, len) != 0) {
        return -1;
    }
    return 0;
}

int fs_autotest_run(void) {
    terminal_writestring("\n[AUTOTEST] fs start");
    log_msg(LOG_INFO, "autotest", "fs_start");

    if (fs_create_dir("/tmp") != 0) {
        fs_at_fail("mkdir_tmp");
        return -1;
    }
    if (fs_create_dir("/tmp/fstest") != 0) {
        fs_at_fail("mkdir_fstest");
        return -1;
    }
    fs_at_ok("mkdir");

    const char* small = "fs-autotest-v1";
    size_t small_len = 14;
    if (fs_at_expect_write_read("/tmp/fstest/a.txt", small, small_len) != 0) {
        fs_at_fail("write_read");
        return -1;
    }
    fs_at_ok("write_read");

    const char* longer = "fs-autotest-v1-longer-content-ok";
    size_t longer_len = 31;
    if (fs_at_expect_write_read("/tmp/fstest/a.txt", longer, longer_len) != 0) {
        fs_at_fail("overwrite_grow");
        return -1;
    }
    fs_at_ok("overwrite_grow");

    const char* shorter = "short";
    size_t shorter_len = 5;
    if (fs_at_expect_write_read("/tmp/fstest/a.txt", shorter, shorter_len) != 0) {
        fs_at_fail("overwrite_shrink");
        return -1;
    }
    fs_at_ok("overwrite_shrink");

    char multi[600];
    for (size_t i = 0; i < sizeof(multi); i++) {
        multi[i] = (char)('A' + (i % 26));
    }
    if (fs_at_expect_write_read("/tmp/fstest/big.bin", multi, sizeof(multi)) != 0) {
        fs_at_fail("multi_sector");
        return -1;
    }
    fs_at_ok("multi_sector");

    if (fs_create_dir("/tmp/fstest/sub") != 0) {
        fs_at_fail("mkdir_nested");
        return -1;
    }
    if (fs_write("/tmp/fstest/sub/b.txt", "nested", 6) != 0) {
        fs_at_fail("write_nested");
        return -1;
    }
    fs_at_ok("nested");

    char listing[512];
    if (fs_list_dir("/tmp/fstest", listing, sizeof(listing)) < 0) {
        fs_at_fail("list");
        return -1;
    }
    if (!fs_at_contains(listing, "a.txt") || !fs_at_contains(listing, "big.bin") ||
        !fs_at_contains(listing, "sub")) {
        fs_at_fail("list_content");
        return -1;
    }
    fs_at_ok("list");

    uint32_t total = 0, used = 0, freeb = 0;
    if (fs_get_disk_usage(&total, &used, &freeb) != 0 || total == 0) {
        fs_at_fail("usage");
        return -1;
    }
    fs_at_ok("usage");

    if (fs_check_integrity() < 0) {
        fs_at_fail("fsck");
        return -1;
    }
    fs_at_ok("fsck");

    if (fs_delete("/tmp/fstest/sub/b.txt") != 0) {
        fs_at_fail("delete_file");
        return -1;
    }
    uint32_t gone = 0;
    if (fs_open("/tmp/fstest/sub/b.txt", &gone) == 0) {
        fs_at_fail("delete_gone");
        return -1;
    }
    if (fs_delete("/tmp/fstest/sub") != 0) {
        fs_at_fail("delete_dir");
        return -1;
    }
    if (fs_delete("/tmp/fstest/a.txt") != 0 || fs_delete("/tmp/fstest/big.bin") != 0) {
        fs_at_fail("cleanup_files");
        return -1;
    }
    if (fs_delete("/tmp/fstest") != 0) {
        fs_at_fail("cleanup_dir");
        return -1;
    }
    fs_at_ok("delete");

    terminal_writestring("\n[AUTOTEST] fs ok");
    log_msg(LOG_INFO, "autotest", "fs_ok");
    return 0;
}
