#include "vfs.h"
#include "fs.h"
#include "mount.h"
#include "utils.h"
#include <string.h>

void vfs_init(void) {
    /* Root mount is established in mount_init(); nothing else required. */
}

static int vfs_abs_path(const char* path, char* out, size_t out_sz) {
    if (!path || !out || out_sz < 2) return -1;
    if (path[0] == '/') {
        size_t i = 0;
        while (path[i] && i + 1 < out_sz) {
            out[i] = path[i];
            i++;
        }
        out[i] = 0;
        return 0;
    }
    return utils_resolve_path(path, out, out_sz);
}

int vfs_resolve(const char* path, bool follow_symlinks, struct vfs_node* out) {
    if (!out) return -1;
    out->mount = get_mount_for_path(path ? path : "/");
    out->mos_index = -1;
    char abs[256];
    if (vfs_abs_path(path ? path : "/", abs, sizeof(abs)) != 0) return -1;
    out->mount = get_mount_for_path(abs);
    if (abs[0] == '/' && abs[1] == 0) {
        out->mos_index = -2;
        return 0;
    }
    int idx = fs_lookup_index(abs, follow_symlinks);
    out->mos_index = idx;
    return (idx >= 0 || idx == -2) ? 0 : -1;
}

int vfs_stat(const char* path, struct fs_stat* st) {
    struct vfs_node n;
    if (vfs_resolve(path, false, &n) != 0) return -1;
    if (n.mos_index == -2) {
        if (!st) return -1;
        memset(st, 0, sizeof(*st));
        st->flags = FS_FLAG_DIRECTORY | FS_FLAG_OCCUPIED;
        st->mode = FS_MODE_DIR;
        st->nlink = 1;
        return 0;
    }
    return fs_index_stat(n.mos_index, st);
}

int vfs_read(const char* path, void* buf, size_t size) {
    return fs_read(path, buf, size);
}

int vfs_write(const char* path, const void* data, size_t size) {
    return fs_write(path, data, size);
}

int vfs_list(const char* path, char* buf, size_t size) {
    return fs_list_dir(path, buf, size);
}

int vfs_unlink(const char* path) { return fs_delete(path); }
int vfs_mkdir(const char* path) { return fs_create_dir(path); }
int vfs_rename(const char* a, const char* b) { return fs_rename(a, b); }
int vfs_symlink(const char* t, const char* l) { return fs_symlink(t, l); }
