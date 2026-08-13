#include "ramfs.h"
#include "mount.h"
#include "serial_log.h"
#include <string.h>

struct ram_node {
    bool used;
    bool is_dir;
    char name[RAMFS_NAME_MAX];
    int parent; /* -1 root of ramfs */
    uint16_t mode;
    uint32_t size;
    uint32_t mtime;
    uint8_t data[RAMFS_FILE_MAX];
};

static struct ram_node nodes[RAMFS_MAX_NODES];
static bool ready;

void ramfs_init(void) {
    memset(nodes, 0, sizeof(nodes));
    nodes[0].used = true;
    nodes[0].is_dir = true;
    nodes[0].name[0] = 0;
    nodes[0].parent = -1;
    nodes[0].mode = FS_MODE_DIR;
    ready = true;
}

bool ramfs_is_tmp_path(const char* path) {
    if (!path) return false;
    if (path[0] == '/' && path[1] == 't' && path[2] == 'm' && path[3] == 'p') {
        if (path[4] == 0 || path[4] == '/') return true;
    }
    return false;
}

void ramfs_strip_tmp(const char* path, char* out, size_t out_sz) {
    if (!out || out_sz < 2) return;
    if (!ramfs_is_tmp_path(path)) {
        out[0] = '/';
        out[1] = 0;
        return;
    }
    const char* p = path + 4;
    if (*p == 0) {
        out[0] = '/';
        out[1] = 0;
        return;
    }
    size_t i = 0;
    while (p[i] && i + 1 < out_sz) {
        out[i] = p[i];
        i++;
    }
    out[i] = 0;
}

static int ram_find_child(int parent, const char* name) {
    for (int i = 1; i < RAMFS_MAX_NODES; i++) {
        if (nodes[i].used && nodes[i].parent == parent && strcmp(nodes[i].name, name) == 0)
            return i;
    }
    return -1;
}

static int ram_alloc(void) {
    for (int i = 1; i < RAMFS_MAX_NODES; i++)
        if (!nodes[i].used) return i;
    return -1;
}

int ramfs_lookup(const char* path, bool follow) {
    (void)follow;
    if (!ready) return -1;
    char rel[128];
    ramfs_strip_tmp(path, rel, sizeof(rel));
    if (rel[0] == '/' && rel[1] == 0) return 0;
    int parent = 0;
    const char* p = rel;
    if (*p == '/') p++;
    char name[RAMFS_NAME_MAX];
    int last = 0;
    while (*p) {
        int i = 0;
        while (*p && *p != '/' && i < RAMFS_NAME_MAX - 1) name[i++] = *p++;
        name[i] = 0;
        if (*p == '/') p++;
        if (!name[0]) continue;
        int idx = ram_find_child(parent, name);
        if (idx < 0) return -1;
        if (*p && !nodes[idx].is_dir) return -1;
        parent = idx;
        last = idx;
    }
    return last;
}

int ramfs_stat(const char* path, struct fs_stat* st) {
    int idx = ramfs_lookup(path, false);
    if (idx < 0 || !st) return -1;
    memset(st, 0, sizeof(*st));
    st->size = nodes[idx].size;
    st->mode = nodes[idx].mode;
    st->mtime = nodes[idx].mtime;
    st->nlink = 1;
    st->flags = FS_FLAG_OCCUPIED | (nodes[idx].is_dir ? FS_FLAG_DIRECTORY : 0);
    return 0;
}

int ramfs_read(const char* path, void* buf, size_t size) {
    int idx = ramfs_lookup(path, true);
    if (idx < 0 || nodes[idx].is_dir) return -1;
    if (size > nodes[idx].size) size = nodes[idx].size;
    memcpy(buf, nodes[idx].data, size);
    return (int)size;
}

int ramfs_write(const char* path, const void* data, size_t size) {
    int idx = ramfs_lookup(path, false);
    if (idx < 0) {
        if (ramfs_create_file(path, FS_MODE_FILE) != 0) return -1;
        idx = ramfs_lookup(path, false);
        if (idx < 0) return -1;
    }
    if (nodes[idx].is_dir) return -1;
    if (size > RAMFS_FILE_MAX) size = RAMFS_FILE_MAX;
    memcpy(nodes[idx].data, data, size);
    nodes[idx].size = (uint32_t)size;
    nodes[idx].mtime = fs_now();
    return 0;
}

int ramfs_list(const char* path, char* buf, size_t size) {
    int dir = ramfs_lookup(path, true);
    if (dir < 0 || !nodes[dir].is_dir || !buf || size == 0) return -1;
    size_t pos = 0;
    buf[0] = 0;
    for (int i = 1; i < RAMFS_MAX_NODES && pos + 1 < size; i++) {
        if (!nodes[i].used || nodes[i].parent != dir) continue;
        const char* n = nodes[i].name;
        while (*n && pos + 1 < size) buf[pos++] = *n++;
        if (nodes[i].is_dir && pos + 1 < size) buf[pos++] = '/';
        if (pos + 1 < size) buf[pos++] = '\n';
    }
    buf[pos] = 0;
    return (int)pos;
}

int ramfs_mkdir(const char* path) {
    if (ramfs_lookup(path, false) >= 0) return 0;
    char rel[128];
    ramfs_strip_tmp(path, rel, sizeof(rel));
    /* parent path */
    char parent_path[128];
    size_t len = 0;
    while (path[len] && len + 1 < sizeof(parent_path)) {
        parent_path[len] = path[len];
        len++;
    }
    parent_path[len] = 0;
    char* slash = strrchr(parent_path, '/');
    if (!slash) return -1;
    const char* base = slash + 1;
    if (slash == parent_path) {
        parent_path[1] = 0;
    } else {
        *slash = 0;
    }
    int parent = ramfs_lookup(parent_path[1] ? parent_path : "/tmp", true);
    if (parent < 0) parent = 0;
    int idx = ram_alloc();
    if (idx < 0) return -1;
    memset(&nodes[idx], 0, sizeof(nodes[idx]));
    nodes[idx].used = true;
    nodes[idx].is_dir = true;
    nodes[idx].parent = parent;
    nodes[idx].mode = FS_MODE_DIR;
    strncpy(nodes[idx].name, base, RAMFS_NAME_MAX - 1);
    nodes[idx].mtime = fs_now();
    return 0;
}

int ramfs_create_file(const char* path, uint16_t mode) {
    if (ramfs_lookup(path, false) >= 0) return 0;
    char parent_path[128];
    size_t len = 0;
    while (path[len] && len + 1 < sizeof(parent_path)) {
        parent_path[len] = path[len];
        len++;
    }
    parent_path[len] = 0;
    char* slash = strrchr(parent_path, '/');
    if (!slash || !slash[1]) return -1;
    const char* base = slash + 1;
    if (slash == parent_path) parent_path[1] = 0;
    else *slash = 0;
    int parent = ramfs_lookup(parent_path[1] ? parent_path : "/tmp", true);
    if (parent < 0) parent = 0;
    int idx = ram_alloc();
    if (idx < 0) return -1;
    memset(&nodes[idx], 0, sizeof(nodes[idx]));
    nodes[idx].used = true;
    nodes[idx].is_dir = false;
    nodes[idx].parent = parent;
    nodes[idx].mode = mode ? mode : FS_MODE_FILE;
    strncpy(nodes[idx].name, base, RAMFS_NAME_MAX - 1);
    nodes[idx].mtime = fs_now();
    return 0;
}

int ramfs_unlink(const char* path) {
    int idx = ramfs_lookup(path, false);
    if (idx <= 0) return -1;
    if (nodes[idx].is_dir) {
        for (int i = 1; i < RAMFS_MAX_NODES; i++)
            if (nodes[i].used && nodes[i].parent == idx) return -1;
    }
    memset(&nodes[idx], 0, sizeof(nodes[idx]));
    return 0;
}

int ramfs_mount_tmp(void) {
    if (!ready) ramfs_init();
    /* Register mount point type 1 = ramfs */
    extern int mount_register(const char* path, int disk_id, uint8_t fs_type);
    if (mount_register("/tmp", -1, 1) != 0) {
        log_msg(LOG_INFO, "ramfs", "tmp mount skip");
        return -1;
    }
    log_msg(LOG_INFO, "ramfs", "mounted /tmp");
    return 0;
}
