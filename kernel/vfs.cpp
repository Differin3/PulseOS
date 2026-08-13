#include "vfs.h"
#include "fs.h"
#include "fs_file.h"
#include "mount.h"
#include "ramfs.h"
#include "utils.h"
#include <string.h>

void vfs_init(void) {
    ramfs_init();
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

static bool is_ramfs_mount(const struct mount_point* m) {
    return m && m->fs_type == FS_TYPE_RAMFS;
}

int vfs_resolve(const char* path, bool follow_symlinks, struct vfs_node* out) {
    if (!out) return -1;
    out->mount = 0;
    out->mos_index = -1;
    char abs[256];
    if (vfs_abs_path(path ? path : "/", abs, sizeof(abs)) != 0) return -1;
    out->mount = get_mount_for_path(abs);
    if (is_ramfs_mount(out->mount)) {
        int idx = ramfs_lookup(abs, follow_symlinks);
        out->mos_index = idx;
        return (idx >= 0) ? 0 : -1;
    }
    if (abs[0] == '/' && abs[1] == 0) {
        out->mos_index = (int)FS_ROOT_INO;
        return 0;
    }
    int idx = fs_lookup_index(abs, follow_symlinks);
    out->mos_index = idx;
    return (idx >= 0) ? 0 : -1;
}

int vfs_stat(const char* path, struct fs_stat* st) {
    char abs[256];
    if (vfs_abs_path(path ? path : "/", abs, sizeof(abs)) != 0) return -1;
    const struct mount_point* m = get_mount_for_path(abs);
    if (is_ramfs_mount(m)) return ramfs_stat(abs, st);
    if (abs[0] == '/' && abs[1] == 0) {
        if (!st) return -1;
        memset(st, 0, sizeof(*st));
        st->flags = FS_FLAG_DIRECTORY | FS_FLAG_OCCUPIED;
        st->mode = FS_MODE_DIR;
        st->nlink = 1;
        return 0;
    }
    return fs_stat(abs, st);
}

int vfs_read(const char* path, void* buf, size_t size) {
    char abs[256];
    if (vfs_abs_path(path, abs, sizeof(abs)) != 0) return -1;
    if (is_ramfs_mount(get_mount_for_path(abs))) return ramfs_read(abs, buf, size);
    return fs_read(abs, buf, size);
}

int vfs_write(const char* path, const void* data, size_t size) {
    char abs[256];
    if (vfs_abs_path(path, abs, sizeof(abs)) != 0) return -1;
    if (is_ramfs_mount(get_mount_for_path(abs))) return ramfs_write(abs, data, size);
    return fs_write(abs, data, size);
}

int vfs_list(const char* path, char* buf, size_t size) {
    char abs[256];
    if (vfs_abs_path(path ? path : "/", abs, sizeof(abs)) != 0) return -1;
    if (is_ramfs_mount(get_mount_for_path(abs))) return ramfs_list(abs, buf, size);
    return fs_list_dir(abs, buf, size);
}

int vfs_unlink(const char* path) {
    char abs[256];
    if (vfs_abs_path(path, abs, sizeof(abs)) != 0) return -1;
    if (is_ramfs_mount(get_mount_for_path(abs))) return ramfs_unlink(abs);
    return fs_delete(abs);
}

int vfs_mkdir(const char* path) {
    char abs[256];
    if (vfs_abs_path(path, abs, sizeof(abs)) != 0) return -1;
    if (is_ramfs_mount(get_mount_for_path(abs))) return ramfs_mkdir(abs);
    return fs_create_dir(abs);
}

int vfs_rename(const char* a, const char* b) { return fs_rename(a, b); }
int vfs_symlink(const char* t, const char* l) { return fs_symlink(t, l); }
int vfs_link(const char* a, const char* b) { return fs_link(a, b); }
