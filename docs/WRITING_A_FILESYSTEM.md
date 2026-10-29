# Writing your own filesystem

1. Open `src/user/studentfs.c` (a complete tiny example).
2. A filesystem is a `struct fs_driver` (name, mount, unmount, mkfs) plus `struct node_ops`
   (read, write, lookup, create, unlink, readdir, truncate, release). Leave unused ones NULL.
3. `mount()` must set `mnt->root` using `vfs_node_alloc(mnt, ino, VFS_DIR, 0, &ops, priv)`.
4. `lookup()` returns a node for a name or `-ENOENT`. Allocate nodes with `vfs_node_alloc`;
   the VFS frees them and calls `release` so you can free `node->priv`.
5. Register with `REGISTER_FS(driver);`
6. Use a disk: `blockdev_read/write` on the `dev` given to `mount` (see include/blockdev.h).
   `src/fs/myfs.c` is a full on-disk example (superblock, bitmap, inodes, directories).
7. Test in the shell: `mount none /h hellofs`, or `mkfs yourfs ram0` then `mount ram0 /mnt yourfs`.

Tip: start read-only (as hellofs), then add `create` and `write`.
