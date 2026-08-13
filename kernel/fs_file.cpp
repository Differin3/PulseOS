#include "fs_file.h"
#include "fs.h"
#include "vfs.h"
#include "utils.h"
#include "sched/task.h"
#include <string.h>

struct open_file {
    bool used;
    int mos_index;
    uint32_t offset;
    int flags;
    char path[128];
};

static struct open_file g_ofiles[FS_OPEN_MAX];

static bool file_is_dir_idx(int idx) {
    const struct fs_file_entry* e = fs_entry_at(idx);
    return e && (e->flags & FS_FLAG_DIRECTORY);
}

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

int vfs_open(const char* path, int flags, uint16_t mode) {
    if (!path || !fs_ready()) return -1;
    char abs[256];
    if (path[0] == '/') {
        size_t i = 0;
        while (path[i] && i + 1 < sizeof(abs)) {
            abs[i] = path[i];
            i++;
        }
        abs[i] = 0;
    } else if (utils_resolve_path(path, abs, sizeof(abs)) != 0) {
        return -1;
    }

    int idx = fs_lookup_index(abs, true);
    if (idx < 0) {
        if (!(flags & O_CREAT)) return -1;
        idx = fs_create_file(abs, mode ? mode : FS_MODE_FILE);
        if (idx < 0) return -1;
    }
    if (file_is_dir_idx(idx)) return -1;

    if (flags & O_TRUNC) {
        if (fs_index_truncate(idx, 0) != 0) return -1;
    }

    int of = of_alloc();
    if (of < 0) return -1;
    g_ofiles[of].mos_index = idx;
    g_ofiles[of].flags = flags ? flags : O_RDWR;
    {
        const struct fs_file_entry* e = fs_entry_at(idx);
        g_ofiles[of].offset = (flags & O_APPEND) && e ? e->size_bytes : 0;
    }
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

int vfs_close(int tfd) {
    return task_fd_close(tfd);
}

int vfs_fread(int tfd, void* buf, size_t size) {
    uint8_t ty = 0;
    int handle = -1;
    if (task_fd_get(tfd, &ty, &handle) != 0 || ty != TASK_FD_FILE) return -1;
    if (handle < 0 || handle >= FS_OPEN_MAX || !g_ofiles[handle].used) return -1;
    int acc = g_ofiles[handle].flags & 0x3;
    if (acc == O_WRONLY) return -1;
    int n = fs_index_read(g_ofiles[handle].mos_index, g_ofiles[handle].offset, buf, size);
    if (n > 0) g_ofiles[handle].offset += (uint32_t)n;
    return n;
}

int vfs_fwrite(int tfd, const void* data, size_t size) {
    uint8_t ty = 0;
    int handle = -1;
    if (task_fd_get(tfd, &ty, &handle) != 0 || ty != TASK_FD_FILE) return -1;
    if (handle < 0 || handle >= FS_OPEN_MAX || !g_ofiles[handle].used) return -1;
    int acc = g_ofiles[handle].flags & 0x3;
    if (acc == O_RDONLY) return -1;
    if (g_ofiles[handle].flags & O_APPEND) {
        const struct fs_file_entry* e = fs_entry_at(g_ofiles[handle].mos_index);
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
    const struct fs_file_entry* e = fs_entry_at(g_ofiles[handle].mos_index);
    if (!e) return -1;
    int32_t base = 0;
    if (whence == SEEK_SET) base = 0;
    else if (whence == SEEK_CUR) base = (int32_t)g_ofiles[handle].offset;
    else if (whence == SEEK_END) base = (int32_t)e->size_bytes;
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
    return fs_index_stat(g_ofiles[handle].mos_index, st);
}
