# KnitOS MOS filesystem

## Overview

MOS (Magic `0x4D4F5320` / `"MOS "`) is the native on-disk filesystem for KnitOS.
**On-disk version 3** uses real inodes + directory entries, multi-extent allocation,
ordered journaling (data then metadata), page cache, and works with VFS + ramfs.

Old v1/v2 images are **recreated** on boot when the layout/version does not match
(see serial log: `old/invalid MOS layout — recreating`).

## Layout (v3)

| Region | Description |
|--------|-------------|
| LBA 0 | Superblock (`fs_boot_sector`, version 3) |
| `inode_table_sector`… | Inode table (`FS_MAX_INODES` = 1024) |
| `free_bitmap_sector`… | Free-sector bitmap |
| `log_start_sector`… | Journal (txn header + up to 32 data sectors) |
| data… | File/dir payloads, indirect extent blocks, xattr |

Root inode is **1** (`FS_ROOT_INO`). Directories store packed dirents
(`ino` + `namelen` + `name`).

### Inode

- `mode`, `uid`, `gid`, `nlink`, `size`, `atime`/`mtime`/`ctime`, `parent`
- flags: file / dir / symlink / fifo / sock / blk / chr (+ `COMPRESSED` reserved)
- **4 direct extents** `{lba,count}` + optional **indirect** extent sector
- short symlink target in `symlink_inline` (else data extents)
- optional `xattr_lba`

## Journal (ordered)

1. Write new **data** sectors (allocate-on-write for growth/overwrite paths).
2. Journal + commit **metadata** (inode table chunk + bitmap).
3. On mount, if `log_next != 0`, `fs_recover()` replays redo then clears the log.

## fsck

`fs_fsck(repair)` rebuilds bitmap consistency, fixes `nlink` from dirents,
drops orphan inodes when repairing. Called on mount after recover.
Markers: `fsck_ok` / `fsck_repaired`.

## Page cache

[`kernel/fs_cache.*`](../kernel/fs_cache.cpp): 256×512 B slots, dirty + LRU-ish
eviction, `fs_sync()` / `fs_cache_sync()` flush. MOS I/O goes through the cache.

## Hard links / attrs

- `fs_link` / unlink with real `nlink`
- `chmod` / `chown` / uid enforce (uid 0 bypass)
- minimal xattr (`fs_setxattr` / `fs_getxattr`)

## VFS / mounts

- MOS on `/`
- **ramfs** on `/tmp` (`FS_TYPE_RAMFS`)
- Open-file layer: `O_EXCL`, `openat`, `dup`, `fcntl`, `getdents`, `fsync`
- Syscalls include `SYS_DUP`, `SYS_LINK`, `SYS_OPENAT`, `SYS_GETDENTS`, `SYS_MMAP_RO` (load-to-buffer)

## Shell

`ls`, `cat`, `write`, `mkdir [-p]`, `rm [-r]`, `mv`, `cp`, `ln` / `ln -s`,
`stat`, `chmod`, `chown`, `touch`, `df`, `du`, `sync`, `mount`, `fsck`, `nano`, `find`

## Limits

- 1024 inodes, multi-extent (fragmented OK), names ≤ 255
- No full transparent compression yet (`FS_FLAG_COMPRESSED` reserved)
- `mmap` RO is a simplified load-into-heap helper, not page-fault mapping

## Autotest

`autotest fs` covers mkdir, R/W, rename, truncate, symlink, hardlink, journal,
cache sync, fd/lseek, O_EXCL, ramfs `/tmp`, getdents, delete.
Markers: `fs_ok`, `symlink_ok`, `journal_ok`, `fd_file_ok`, `extent_ok`,
`hardlink_ok`, `fsck_ok`, `cache_ok`, `o_excl_ok`, `mount_tmp_ok`, `getdents_ok`.
