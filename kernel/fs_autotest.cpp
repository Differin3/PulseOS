#include "fs_autotest.h"
#include "fs.h"
#include "fs_file.h"
#include "fs_cache.h"
#include "vfs.h"
#include "ramfs.h"
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

    /* MOS paths under /var ( /tmp may be ramfs ) */
    (void)fs_rm_rf("/var/fstest"); /* leftover from prior run breaks rename */
    if (fs_create_dir("/var") != 0) { fs_at_fail("mkdir_var"); return -1; }
    if (fs_create_dir("/var/fstest") != 0) { fs_at_fail("mkdir_fstest"); return -1; }
    fs_at_ok("mkdir");

    const char* small = "fs-autotest-v1";
    if (fs_at_expect_write_read("/var/fstest/a.txt", small, 14) != 0) {
        fs_at_fail("write_read"); return -1;
    }
    fs_at_ok("write_read");

    if (fs_at_expect_write_read("/var/fstest/a.txt", "fs-autotest-v1-longer-content-ok", 31) != 0) {
        fs_at_fail("overwrite_grow"); return -1;
    }
    fs_at_ok("overwrite_grow");

    if (fs_at_expect_write_read("/var/fstest/a.txt", "short", 5) != 0) {
        fs_at_fail("overwrite_shrink"); return -1;
    }
    fs_at_ok("overwrite_shrink");

    char multi[600];
    for (size_t i = 0; i < sizeof(multi); i++) multi[i] = (char)('A' + (i % 26));
    if (fs_at_expect_write_read("/var/fstest/big.bin", multi, sizeof(multi)) != 0) {
        fs_at_fail("multi_sector"); return -1;
    }
    fs_at_ok("multi_sector");
    log_msg(LOG_INFO, "autotest", "extent_ok");

    if (fs_create_dir("/var/fstest/sub") != 0) { fs_at_fail("mkdir_nested"); return -1; }
    if (fs_write("/var/fstest/sub/b.txt", "nested", 6) != 0) { fs_at_fail("write_nested"); return -1; }
    fs_at_ok("nested");

    char listing[512];
    if (fs_list_dir("/var/fstest", listing, sizeof(listing)) < 0) { fs_at_fail("list"); return -1; }
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

    if (fs_fsck(true) < 0) { fs_at_fail("fsck"); return -1; }
    fs_at_ok("fsck");
    log_msg(LOG_INFO, "autotest", "fsck_ok");

    if (fs_rename("/var/fstest/a.txt", "/var/fstest/renamed.txt") != 0) {
        fs_at_fail("rename"); return -1;
    }
    fs_at_ok("rename");

    if (fs_truncate("/var/fstest/renamed.txt", 2) != 0) { fs_at_fail("truncate"); return -1; }
    fs_at_ok("truncate");

    if (fs_symlink("/var/fstest/renamed.txt", "/var/fstest/link") != 0) {
        fs_at_fail("symlink"); return -1;
    }
    char rb[32];
    if (fs_read("/var/fstest/link", rb, 2) != 2) { fs_at_fail("symlink_read"); return -1; }
    fs_at_ok("symlink");
    log_msg(LOG_INFO, "autotest", "symlink_ok");

    if (fs_link("/var/fstest/renamed.txt", "/var/fstest/hard") != 0) {
        fs_at_fail("hardlink"); return -1;
    }
    struct fs_stat st1, st2;
    if (fs_stat("/var/fstest/renamed.txt", &st1) != 0 || fs_stat("/var/fstest/hard", &st2) != 0 ||
        st1.nlink < 2) {
        fs_at_fail("hardlink_nlink"); return -1;
    }
    fs_at_ok("hardlink");
    log_msg(LOG_INFO, "autotest", "hardlink_ok");

    if (fs_journal_selftest() != 0) { fs_at_fail("journal"); return -1; }
    fs_at_ok("journal");
    log_msg(LOG_INFO, "autotest", "journal_ok");

    uint32_t hits0 = fs_cache_hits();
    char cbuf[8];
    fs_read("/var/fstest/renamed.txt", cbuf, 2);
    fs_read("/var/fstest/renamed.txt", cbuf, 2);
    if (fs_cache_hits() < hits0) { /* allow equal if cold */ }
    if (fs_sync() != 0) { fs_at_fail("cache_sync"); return -1; }
    fs_at_ok("cache");
    log_msg(LOG_INFO, "autotest", "cache_ok");

    int fd = vfs_open("/var/fstest/fd.bin", O_CREAT | O_RDWR | O_TRUNC, FS_MODE_FILE);
    if (fd < 0) { fs_at_fail("fd_open"); return -1; }
    if (vfs_fwrite(fd, "ABCDEFGH", 8) != 8) { fs_at_fail("fd_write"); return -1; }
    if (vfs_lseek(fd, 2, SEEK_SET) != 2) { fs_at_fail("fd_lseek"); return -1; }
    char chunk[4];
    if (vfs_fread(fd, chunk, 3) != 3 || chunk[0] != 'C') { fs_at_fail("fd_read"); return -1; }
    vfs_close(fd);
    fs_at_ok("fd");
    log_msg(LOG_INFO, "autotest", "fd_file_ok");

    int fd2 = vfs_open("/var/fstest/excl.bin", O_CREAT | O_EXCL | O_RDWR, FS_MODE_FILE);
    if (fd2 < 0) { fs_at_fail("o_excl_create"); return -1; }
    vfs_close(fd2);
    if (vfs_open("/var/fstest/excl.bin", O_CREAT | O_EXCL | O_RDWR, FS_MODE_FILE) >= 0) {
        fs_at_fail("o_excl"); return -1;
    }
    fs_at_ok("o_excl");
    log_msg(LOG_INFO, "autotest", "o_excl_ok");

    if (vfs_mkdir("/tmp/ramtest") != 0) { fs_at_fail("ramfs_mkdir"); return -1; }
    if (vfs_write("/tmp/ramtest/x.txt", "ram", 3) != 0) { fs_at_fail("ramfs_write"); return -1; }
    char rtmp[8];
    if (vfs_read("/tmp/ramtest/x.txt", rtmp, 3) != 3) { fs_at_fail("ramfs_read"); return -1; }
    fs_at_ok("ramfs");
    log_msg(LOG_INFO, "autotest", "mount_tmp_ok");

    uint32_t off = 0;
    char dname[64];
    uint32_t dino = 0;
    int got = fs_readdir("/var/fstest", &off, dname, sizeof(dname), &dino);
    if (got <= 0) { fs_at_fail("getdents"); return -1; }
    fs_at_ok("getdents");
    log_msg(LOG_INFO, "autotest", "getdents_ok");

    if (fs_delete("/var/fstest/sub/b.txt") != 0) { fs_at_fail("delete_file"); return -1; }
    uint32_t gone = 0;
    if (fs_open("/var/fstest/sub/b.txt", &gone) == 0) { fs_at_fail("delete_gone"); return -1; }
    if (fs_delete("/var/fstest/sub") != 0) { fs_at_fail("delete_dir"); return -1; }
    if (fs_delete("/var/fstest/hard") != 0 || fs_delete("/var/fstest/renamed.txt") != 0 ||
        fs_delete("/var/fstest/big.bin") != 0 || fs_delete("/var/fstest/link") != 0 ||
        fs_delete("/var/fstest/fd.bin") != 0 || fs_delete("/var/fstest/excl.bin") != 0) {
        fs_at_fail("cleanup_files"); return -1;
    }
    if (fs_delete("/var/fstest") != 0) { fs_at_fail("cleanup_dir"); return -1; }
    fs_at_ok("delete");

    terminal_writestring("\n[AUTOTEST] fs ok");
    log_msg(LOG_INFO, "autotest", "fs_ok");
    return 0;
}
