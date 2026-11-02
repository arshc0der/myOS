# MyFS - the MyOS filesystem

MyFS (`src/fs/myfs.c`) is a small, complete on-disk filesystem. It is the reference
for writing your own (see `WRITING_A_FILESYSTEM.md`). This page describes what is
on the disk and how the pieces fit.

## Try it

```
myos:/$ ramdisk 4096           # or use a real disk partition
myos:/$ mkfs myfs ram0         # write an empty MyFS
myos:/$ mount ram0 /mnt myfs
myos:/$ cp /etc/x /mnt/        # normal commands now work on it
```

In the desktop, open **Files** and go to `mnt`: you can create folders and files,
rename, copy, cut, paste and delete there exactly like on any other filesystem
(the Files app only uses the VFS, so it does not care which filesystem it is on).

## Disk layout

A block is 1024 bytes (2 sectors of 512 bytes).

| Blocks | Content |
|--------|---------|
| 0 | superblock: magic `MYFS`, version, sizes, where each area starts, free counters |
| 1 .. | block bitmap: one bit per block (1 = used) |
| then | inode table: 64-byte inodes, 16 per block |
| then | data blocks: file contents and directory entries |

### Inode (64 bytes)

| Field | Meaning |
|-------|---------|
| type | 0 free, 1 file, 2 directory |
| links | link count |
| size | size in bytes |
| direct[12] | the first 12 data blocks |
| indirect | one block holding 256 more block numbers |
| mtime | modification time |

Largest file: (12 + 256) x 1024 = 268 KiB.

### Directory

A directory is a file whose data is an array of 32-byte entries:
`{ inode number (4 bytes), name (28 bytes) }`. Inode number 0 means "unused slot".
File names are therefore at most 27 characters on MyFS (the VFS allows 59 for
other filesystems; the Files app shows an error when a name is too long).

### Write-through

Every change is written to disk immediately, so nothing needs flushing at
unmount except the superblock counters.

## How the VFS operations map onto the disk

| VFS operation | MyFS function | What it does |
|---------------|---------------|--------------|
| lookup | `myfs_lookup` | scan the directory for the name, load the inode |
| create | `myfs_create` | allocate an inode (and a block for directories), add a directory entry |
| read / write | `myfs_read`, `myfs_write` | `bmap()` turns a file position into a disk block, allocating on write |
| truncate | `myfs_truncate` | free blocks past the new size |
| unlink | `myfs_unlink` | remove the entry; free the inode and its blocks (directories must be empty) |
| readdir | `myfs_readdir` | return the n-th used entry |
| mkfs / mount / unmount | `myfs_mkfs`, `myfs_mount`, `myfs_unmount` | format, read superblock + bitmap, write counters back |

## Higher-level operations (`include/fileops.h`)

The VFS only knows single files. Copying a folder, moving between filesystems,
deleting a folder with contents and renaming are provided once, in `src/fs/fileops.c`,
and used by the Files app and by your own programs:

```c
fo_copy("/mnt/a", "/tmp/a");        // file or whole folder, any filesystem to any other
fo_move("/tmp/a", "/mnt/b");        // copy, then delete the source
fo_remove("/mnt/old");              // file or folder with everything inside
fo_rename("/mnt/b", "c");           // same folder, new name
fo_new_dir("/mnt/new");  fo_new_file("/mnt/new/readme.txt");
fo_unique_path("/mnt", "untitled folder", out, sizeof out);   // "untitled folder 2" if taken
```

All of them return 0 or a negative error (`-EEXIST`, `-ENOENT`, `-ENOSPC`, ...).
A copy that fails half way removes the partial file.

## Ideas for your own version

- A double-indirect block to allow files larger than 268 KiB.
- Longer names (grow `struct myfs_dirent`; change `DIRENT_SIZE` and `MYFS_NAME_MAX`).
- A journal or a write-back cache.
- Timestamps shown in the Files app (the inode already has `mtime`).
