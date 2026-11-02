/*
 * fileops.h - ready-made file operations on top of the VFS.
 *
 * The VFS (vfs.h) gives you open/read/write/mkdir/unlink. Programs such as the
 * Files app also need "copy a folder", "move", "delete a folder with
 * everything inside" and "pick a name that is not taken yet". They are here so
 * every program (GUI, shell, your own) does them the same way, and they work
 * between different filesystems (ramfs, myfs, your own).
 */
#ifndef FILEOPS_H
#define FILEOPS_H
#include "types.h"

/* Copy a file or a whole directory tree. `dst` must not exist. Returns 0 or -errno. */
int  fo_copy(const char *src, const char *dst);
/* Move (rename). Copies, then deletes the source. Returns 0 or -errno. */
int  fo_move(const char *src, const char *dst);
/* Delete a file, or a directory and everything inside it. Returns 0 or -errno. */
int  fo_remove(const char *path);
/* Rename inside the same directory: new_name has no slash. Returns 0 or -errno. */
int  fo_rename(const char *path, const char *new_name);
/* Create an empty file / a directory. Returns 0 or -errno (-EEXIST if it is there). */
int  fo_new_file(const char *path);
int  fo_new_dir(const char *path);
/* Find a free name in `dir` that starts with `base` ("untitled folder",
 * "untitled folder 2", ...). Writes the full path to out. */
void fo_unique_path(const char *dir, const char *base, char *out, size_t n);
/* Like fo_unique_path, but keeps the extension: "a.txt" -> "a copy.txt". */
void fo_copy_name(const char *dir, const char *name, char *out, size_t n);
/* Last component of a path ("/a/b" -> "b"). */
const char *fo_basename(const char *path);
/* dir + "/" + name (no double slash for "/"). */
void fo_join(char *out, size_t n, const char *dir, const char *name);
/* true if `path` exists. */
bool fo_exists(const char *path);

#endif
