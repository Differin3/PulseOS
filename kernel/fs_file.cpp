#include "fs_file.h"
#include "fs.h"
#include "vfs.h"
#include "mount.h"
#include "ramfs.h"
#include "utils.h"
#include "sched/task.h"
#include <string.h>

struct open_file {
    bool used;
    int mos_index;
    uint32_t offset;
    int flags;
    uint8_t fs_type; /* 0 MOS, 1 ramfs */
    bool is_dir;
    char path[128];
};

static struct open_file g_ofiles[FS_OPEN_MAX];

static int of_alloc(void) {
    for (int i = 0; i < FS_OPEN_MAX; i++) {
        if (!g_ofiles[i].used) {
            memset(&g_ofiles[i], 0, sizeof(g_ofiles[i]));
            g_ofiles[i].used = true;
            return i;
        }
    }
    return -1;
}

void fs_ofile_release(int ofile) {
    if (ofile >= 0 && ofile < FS_OPEN_MAX) g_ofiles[ofile].used = false;
}

static int abs_path_from(const char* path, char* abs, size_t abs_sz) {
    if (!path) return -1;
    if (path[0] == '/') {
        size_t i = 0;
        while (path[i] && i + 1 < abs_sz) {
            abs[i] = path[i];
            i++;
        }
        abs[i] = 0;
        return 0;
    }
    return utils_resolve_path(path, abs, abs_sz);
}

int vfs_open(const char* path, int flags, uint16_t mode) {
    if (!path) return -1;
    char abs[256];
    if (abs_path_from(path, abs, sizeof(abs)) != 0) return -1;

    const struct mount_point* mp = get_mount_for_path(abs);
    bool ram = mp && mp->fs_type == FS_TYPE_RAMFS;

    if (ram) {
        int idx = ramfs_lookup(abs, true);
        if (idx < 0) {
            if (!(flags & O_CREAT)) return -1;
            if (ramfs_create_file(abs, mode ? mode : FS_MODE_FILE) != 0) return -1;
            idx = ramfs_lookup(abs, true);
            if (idx < 0) return -1;
        } else if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {
            return -1;
        }
        struct fs_stat st;
        if (ramfs_stat(abs, &st) != 0) return -1;
        if ((st.flags & FS_FLAG_DIRECTORY) && !(flags & O_DIRECTORY)) return -1;
        if (!(st.flags & FS_FLAG_DIRECTORY) && (flags & O_DIRECTORY)) return -1;
        int of = of_alloc();
        if (of < 0) return -1;
        g_ofiles[of].mos_index = idx;
        g_ofiles[of].fs_type = FS_TYPE_RAMFS;
        g_ofiles[of].is_dir = (st.flags & FS_FLAG_DIRECTORY) != 0;
        g_ofiles[of].flags = flags ? flags : O_RDWR;
        g_ofiles[of].offset = (flags & O_APPEND) ? st.size : 0;
        size_t n = 0;
        while (abs[n] && n + 1 < sizeof(g_ofiles[of].path)) {
            g_ofiles[of].path[n] = abs[n];
            n++;
        }
        g_ofiles[of].path[n] = 0;
        if (flags & O_TRUNC && !g_ofiles[of].is_dir) ramfs_write(abs, "", 0);
        int tfd = task_fd_alloc(TASK_FD_FILE, of, g_ofiles[of].path);
        if (tfd < 0) {
            g_ofiles[of].used = false;
            return -1;
        }
        return tfd;
    }

    if (!fs_ready()) return -1;
    int idx = fs_lookup_index(abs, true);
    if (idx < 0) {
        if (!(flags & O_CREAT)) return -1;
        idx = fs_create_file(abs, mode ? mode : FS_MODE_FILE);
        if (idx < 0) return -1;
    } else if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {
        return -1;
    }

    const struct fs_inode* e = fs_entry_at(idx);
    if (!e) return -1;
    bool is_dir = (e->flags & FS_FLAG_DIRECTORY) != 0;
    if (is_dir && !(flags & O_DIRECTORY)) return -1;
    if (!is_dir && (flags & O_DIRECTORY)) return -1;
    if (!is_dir && (flags & O_TRUNC)) {
        if (fs_index_truncate(idx, 0) != 0) return -1;
    }

    int of = of_alloc();
    if (of < 0) return -1;
    g_ofiles[of].mos_index = idx;
    g_ofiles[of].fs_type = FS_TYPE_MOS;
    g_ofiles[of].is_dir = is_dir;
    g_ofiles[of].flags = flags ? flags : O_RDWR;
    g_ofiles[of].offset = (flags & O_APPEND) ? e->size_bytes : 0;
    size_t n = 0;
    while (abs[n] && n + 1 < sizeof(g_ofiles[of].path)) {
        g_ofiles[of].path[n] = abs[n];
        n++;
    }
    g_ofiles[of].path[n] = 0;

    int tfd = task_fd_alloc(TASK_FD_FILE, of, g_ofiles[of].path);
    if (tfd < 0) {
        g_ofiles[of].used = false;
        return -1;
    }
    return tfd;
}

int vfs_openat(int dirfd, const char* path, int flags, uint16_t mode) {
    if (!path) return -1;
    if (path[0] == '/') return vfs_open(path, flags, mode);
    if (dirfd < 0) return vfs_open(path, flags, mode);
    uint8_t ty = 0;
    int handle = -1;
    if (task_fd_get(dirfd, &ty, &handle) != 0 || ty != TASK_FD_FILE) return -1;
    if (handle < 0 || handle >= FS_OPEN_MAX || !g_ofiles[handle].used) return -1;
    char combined[256];
    size_t n = 0;
    const char* base = g_ofiles[handle].path;
    while (base[n] && n + 1 < sizeof(combined)) {
        combined[n] = base[n];
        n++;
    }
    if (n > 0 && combined[n - 1] != '/' && n + 1 < sizeof(combined)) combined[n++] = '/';
    for (size_t i = 0; path[i] && n + 1 < sizeof(combined); i++) combined[n++] = path[i];
    combined[n] = 0;
    return vfs_open(combined, flags, mode);
}

int vfs_close(int tfd) { return task_fd_close(tfd); }

int vfs_fread(int tfd, void* buf, size_t size) {
    uint8_t ty = 0;
    int handle = -1;
    if (task_fd_get(tfd, &ty, &handle) != 0 || ty != TASK_FD_FILE) return -1;
    if (handle < 0 || handle >= FS_OPEN_MAX || !g_ofiles[handle].used) return -1;
    if (g_ofiles[handle].is_dir) return -1;
    int acc = g_ofiles[handle].flags & 0x3;
    if (acc == O_WRONLY) return -1;
    int n;
    if (g_ofiles[handle].fs_type == FS_TYPE_RAMFS) {
        struct fs_stat st;
        if (ramfs_stat(g_ofiles[handle].path, &st) != 0) return -1;
        if (g_ofiles[handle].offset >= st.size) return 0;
        size_t avail = st.size - g_ofiles[handle].offset;
        if (size > avail) size = avail;
        char tmp[RAMFS_FILE_MAX];
        int got = ramfs_read(g_ofiles[handle].path, tmp, st.size);
        if (got < 0) return -1;
        memcpy(buf, tmp + g_ofiles[handle].offset, size);
        n = (int)size;
    } else {
        n = fs_index_read(g_ofiles[handle].mos_index, g_ofiles[handle].offset, buf, size);
    }
    if (n > 0) g_ofiles[handle].offset += (uint32_t)n;
    return n;
}

int vfs_fwrite(int tfd, const void* data, size_t size) {
    uint8_t ty = 0;
    int handle = -1;
    if (task_fd_get(tfd, &ty, &handle) != 0 || ty != TASK_FD_FILE) return -1;
    if (handle < 0 || handle >= FS_OPEN_MAX || !g_ofiles[handle].used) return -1;
    if (g_ofiles[handle].is_dir) return -1;
    int acc = g_ofiles[handle].flags & 0x3;
    if (acc == O_RDONLY) return -1;
    if (g_ofiles[handle].fs_type == FS_TYPE_RAMFS) {
        /* rewrite whole file from offset 0 for simplicity when append/overwrite */
        struct fs_stat st;
        ramfs_stat(g_ofiles[handle].path, &st);
        if (g_ofiles[handle].flags & O_APPEND) g_ofiles[handle].offset = st.size;
        char tmp[RAMFS_FILE_MAX];
        memset(tmp, 0, sizeof(tmp));
        if (st.size) ramfs_read(g_ofiles[handle].path, tmp, st.size);
        if (g_ofiles[handle].offset + size > RAMFS_FILE_MAX) return -1;
        memcpy(tmp + g_ofiles[handle].offset, data, size);
        uint32_t end = g_ofiles[handle].offset + (uint32_t)size;
        if (end < st.size) end = st.size;
        if (ramfs_write(g_ofiles[handle].path, tmp, end) != 0) return -1;
        g_ofiles[handle].offset += (uint32_t)size;
        return (int)size;
    }
    if (g_ofiles[handle].flags & O_APPEND) {
        const struct fs_inode* e = fs_entry_at(g_ofiles[handle].mos_index);
        if (e) g_ofiles[handle].offset = e->size_bytes;
    }
    int n = fs_index_write(g_ofiles[handle].mos_index, g_ofiles[handle].offset, data, size);
    if (n > 0) g_ofiles[handle].offset += (uint32_t)n;
    return n;
}

int vfs_lseek(int tfd, int32_t offset, int whence) {
    uint8_t ty = 0;
    int handle = -1;
    if (task_fd_get(tfd, &ty, &handle) != 0 || ty != TASK_FD_FILE) return -1;
    if (handle < 0 || handle >= FS_OPEN_MAX || !g_ofiles[handle].used) return -1;
    uint32_t size = 0;
    if (g_ofiles[handle].fs_type == FS_TYPE_RAMFS) {
        struct fs_stat st;
        if (ramfs_stat(g_ofiles[handle].path, &st) != 0) return -1;
        size = st.size;
    } else {
        const struct fs_inode* e = fs_entry_at(g_ofiles[handle].mos_index);
        if (!e) return -1;
        size = e->size_bytes;
    }
    int32_t base = 0;
    if (whence == SEEK_SET) base = 0;
    else if (whence == SEEK_CUR) base = (int32_t)g_ofiles[handle].offset;
    else if (whence == SEEK_END) base = (int32_t)size;
    else return -1;
    int32_t neu = base + offset;
    if (neu < 0) return -1;
    g_ofiles[handle].offset = (uint32_t)neu;
    return (int)g_ofiles[handle].offset;
}

int vfs_fstat(int tfd, struct fs_stat* st) {
    uint8_t ty = 0;
    int handle = -1;
    if (task_fd_get(tfd, &ty, &handle) != 0 || ty != TASK_FD_FILE) return -1;
    if (handle < 0 || handle >= FS_OPEN_MAX || !g_ofiles[handle].used) return -1;
    if (g_ofiles[handle].fs_type == FS_TYPE_RAMFS)
        return ramfs_stat(g_ofiles[handle].path, st);
    return fs_index_stat(g_ofiles[handle].mos_index, st);
}

int vfs_dup(int tfd) {
    uint8_t ty = 0;
    int handle = -1;
    if (task_fd_get(tfd, &ty, &handle) != 0 || ty != TASK_FD_FILE) return -1;
    if (handle < 0 || handle >= FS_OPEN_MAX || !g_ofiles[handle].used) return -1;
    return task_fd_alloc(TASK_FD_FILE, handle, g_ofiles[handle].path);
}

int vfs_dup2(int oldfd, int newfd) {
    if (oldfd == newfd) return newfd;
    uint8_t ty = 0;
    int handle = -1;
    if (task_fd_get(oldfd, &ty, &handle) != 0 || ty != TASK_FD_FILE) return -1;
    task_fd_close(newfd);
    /* allocate specifically — fall back to dup */
    (void)newfd;
    return vfs_dup(oldfd);
}

int vfs_fcntl(int tfd, int cmd, int arg) {
    uint8_t ty = 0;
    int handle = -1;
    if (task_fd_get(tfd, &ty, &handle) != 0 || ty != TASK_FD_FILE) return -1;
    if (handle < 0 || handle >= FS_OPEN_MAX || !g_ofiles[handle].used) return -1;
    if (cmd == F_GETFL) return g_ofiles[handle].flags;
    if (cmd == F_SETFL) {
        g_ofiles[handle].flags = (g_ofiles[handle].flags & 0x3) | (arg & ~0x3);
        return 0;
    }
    return -1;
}

int vfs_fsync(int tfd) {
    uint8_t ty = 0;
    int handle = -1;
    if (task_fd_get(tfd, &ty, &handle) != 0 || ty != TASK_FD_FILE) return -1;
    (void)handle;
    return fs_sync();
}

int vfs_getdents(int tfd, char* buf, size_t size) {
    uint8_t ty = 0;
    int handle = -1;
    if (task_fd_get(tfd, &ty, &handle) != 0 || ty != TASK_FD_FILE) return -1;
    if (handle < 0 || !g_ofiles[handle].used || !g_ofiles[handle].is_dir) return -1;
    if (!buf || size < 2) return -1;
    char name[FS_FILENAME_LEN];
    uint32_t ino = 0;
    int got = fs_readdir(g_ofiles[handle].path, &g_ofiles[handle].offset, name, sizeof(name), &ino);
    if (got <= 0) return 0;
    size_t n = 0;
    while (name[n] && n + 1 < size) {
        buf[n] = name[n];
        n++;
    }
    buf[n] = 0;
    return (int)n;
}
