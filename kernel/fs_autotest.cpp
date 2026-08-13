#include "fs_autotest.h"
#include "fs.h"
#include "fs_file.h"
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
    if (fs_write(path, data, len) != 0) return -1;
    uint32_t size = 0;
    if (fs_open(path, &size) != 0) return -1;
    if ((size_t)size != len) return -1;
    char buf[1024];
    if (len > sizeof(buf)) return -1;
    if (fs_read(path, buf, len) < 0) return -1;
    if (memcmp(buf, data, len) != 0) return -1;
    return 0;
}

int fs_autotest_run(void) {
    terminal_writestring("\n[AUTOTEST] fs start");
    log_msg(LOG_INFO, "autotest", "fs_start");

    if (fs_create_dir("/tmp") != 0) { fs_at_fail("mkdir_tmp"); return -1; }
    if (fs_create_dir("/tmp/fstest") != 0) { fs_at_fail("mkdir_fstest"); return -1; }
    fs_at_ok("mkdir");

    const char* small = "fs-autotest-v1";
    if (fs_at_expect_write_read("/tmp/fstest/a.txt", small, 14) != 0) {
        fs_at_fail("write_read"); return -1;
    }
    fs_at_ok("write_read");

    if (fs_at_expect_write_read("/tmp/fstest/a.txt", "fs-autotest-v1-longer-content-ok", 31) != 0) {
        fs_at_fail("overwrite_grow"); return -1;
    }
    fs_at_ok("overwrite_grow");

    if (fs_at_expect_write_read("/tmp/fstest/a.txt", "short", 5) != 0) {
        fs_at_fail("overwrite_shrink"); return -1;
    }
    fs_at_ok("overwrite_shrink");

    char multi[600];
    for (size_t i = 0; i < sizeof(multi); i++) multi[i] = (char)('A' + (i % 26));
    if (fs_at_expect_write_read("/tmp/fstest/big.bin", multi, sizeof(multi)) != 0) {
        fs_at_fail("multi_sector"); return -1;
    }
    fs_at_ok("multi_sector");

    if (fs_create_dir("/tmp/fstest/sub") != 0) { fs_at_fail("mkdir_nested"); return -1; }
    if (fs_write("/tmp/fstest/sub/b.txt", "nested", 6) != 0) { fs_at_fail("write_nested"); return -1; }
    fs_at_ok("nested");

    char listing[512];
    if (fs_list_dir("/tmp/fstest", listing, sizeof(listing)) < 0) { fs_at_fail("list"); return -1; }
    if (!fs_at_contains(listing, "a.txt") || !fs_at_contains(listing, "big.bin") ||
        !fs_at_contains(listing, "sub")) {
        fs_at_fail("list_content"); return -1;
    }
    fs_at_ok("list");

    uint32_t total = 0, used = 0, freeb = 0;
    if (fs_get_disk_usage(&total, &used, &freeb) != 0 || total == 0) {
        fs_at_fail("usage"); return -1;
    }
    fs_at_ok("usage");

    if (fs_check_integrity() < 0) { fs_at_fail("fsck"); return -1; }
    fs_at_ok("fsck");

    if (fs_rename("/tmp/fstest/a.txt", "/tmp/fstest/renamed.txt") != 0) {
        fs_at_fail("rename"); return -1;
    }
    fs_at_ok("rename");

    if (fs_truncate("/tmp/fstest/renamed.txt", 2) != 0) { fs_at_fail("truncate"); return -1; }
    fs_at_ok("truncate");

    if (fs_symlink("/tmp/fstest/renamed.txt", "/tmp/fstest/link") != 0) {
        fs_at_fail("symlink"); return -1;
    }
    char rb[32];
    if (fs_read("/tmp/fstest/link", rb, 2) != 2) { fs_at_fail("symlink_read"); return -1; }
    fs_at_ok("symlink");
    log_msg(LOG_INFO, "autotest", "symlink_ok");

    if (fs_journal_selftest() != 0) { fs_at_fail("journal"); return -1; }
    fs_at_ok("journal");
    log_msg(LOG_INFO, "autotest", "journal_ok");

    int fd = vfs_open("/tmp/fstest/fd.bin", O_CREAT | O_RDWR | O_TRUNC, FS_MODE_FILE);
    if (fd < 0) { fs_at_fail("fd_open"); return -1; }
    if (vfs_fwrite(fd, "ABCDEFGH", 8) != 8) { fs_at_fail("fd_write"); return -1; }
    if (vfs_lseek(fd, 2, SEEK_SET) != 2) { fs_at_fail("fd_lseek"); return -1; }
    char chunk[4];
    if (vfs_fread(fd, chunk, 3) != 3 || chunk[0] != 'C') { fs_at_fail("fd_read"); return -1; }
    vfs_close(fd);
    fs_at_ok("fd");
    log_msg(LOG_INFO, "autotest", "fd_file_ok");

    if (fs_delete("/tmp/fstest/sub/b.txt") != 0) { fs_at_fail("delete_file"); return -1; }
    uint32_t gone = 0;
    if (fs_open("/tmp/fstest/sub/b.txt", &gone) == 0) { fs_at_fail("delete_gone"); return -1; }
    if (fs_delete("/tmp/fstest/sub") != 0) { fs_at_fail("delete_dir"); return -1; }
    if (fs_delete("/tmp/fstest/renamed.txt") != 0 || fs_delete("/tmp/fstest/big.bin") != 0 ||
        fs_delete("/tmp/fstest/link") != 0 || fs_delete("/tmp/fstest/fd.bin") != 0) {
        fs_at_fail("cleanup_files"); return -1;
    }
    if (fs_delete("/tmp/fstest") != 0) { fs_at_fail("cleanup_dir"); return -1; }
    fs_at_ok("delete");

    terminal_writestring("\n[AUTOTEST] fs ok");
    log_msg(LOG_INFO, "autotest", "fs_ok");
    return 0;
}
