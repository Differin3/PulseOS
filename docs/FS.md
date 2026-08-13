# KnitOS MOS filesystem

## Overview

MOS (Magic `0x4D4F5320` / `"MOS "`) is the native on-disk filesystem for KnitOS.
**On-disk version 2** adds mode/mtime/nlink, a write-ahead journal for metadata,
and works with a thin VFS + POSIX-like open-file (fd) layer.

Old v1 images are **recreated** on boot when the layout/version does not match
(see serial log: `old/invalid MOS layout — recreating`).

## Layout

| Region | Description |
|--------|-------------|
| LBA 0 | Superblock (`fs_boot_sector`) |
| `file_table_sector`… | Flat inode/file table (`FS_MAX_FILES` = 256) |
| `free_bitmap_sector`… | Free-sector bitmap |
| `log_start_sector`… | Journal (txn header + up to 16 data sectors) |
| data… | File payloads (contiguous extents) |

Root is virtual (`parent_dir = 0xFFFFFFFF`); directories are table entries with
`FS_FLAG_DIRECTORY`.

## Journal

- **Metadata only** (file table chunk + first bitmap sector) via redo log.
- Transaction: write header+copies to log → set `log_next` → write live sectors → clear `log_next`.
- On mount, if `log_next != 0`, `fs_recover()` replays redo then clears the log.
- File **data** is written after metadata allocation; not fully journaled.

## Attributes

- `mode` (e.g. 0644 / 0755), `mtime` (seconds since boot via PIT), `nlink`
- Symlinks: `FS_FLAG_SYMLINK`, payload = target path; resolve depth ≤ 8

## VFS / fd

- `vfs_resolve` / path helpers in `kernel/vfs.*`
- Open files: `vfs_open` / `vfs_fread` / `vfs_fwrite` / `vfs_lseek` / `vfs_close` (`kernel/fs_file.*`)
- Task `TASK_FD_FILE` holds open-file handle; syscalls `SYS_OPEN/READ/WRITE/CLOSE/LSEEK/STAT/FSTAT`

## Shell

`ls`, `ls -l`, `cat`, `write`, `mkdir [-p]`, `rm [-r]`, `mv`, `cp`, `ln -s`, `stat`, `nano`, `find`

## Limits

- 256 files/dirs total, contiguous extents, no page cache / mmap
- Single root mount (`mount` table ready for future fs_types)

## Autotest

`autotest fs` covers mkdir, R/W, rename, truncate, symlink, journal selftest, fd/lseek, delete.
Markers: `fs_ok`, `symlink_ok`, `journal_ok`, `fd_file_ok`.
