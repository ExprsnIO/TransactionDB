/*
** tdb_file.h — minimal virtual file system (VFS) abstraction.
**
** All disk access goes through a `tdb_vfs` table of function pointers. Two
** backends are provided: the default OS-backed VFS and an in-memory VFS used
** for testing (including fault/crash injection). A future networked or mmap
** backend can be added without touching the pager.
*/
#ifndef TDB_FILE_H
#define TDB_FILE_H

#include "../common/tdb_internal.h"

/* Lock levels (advisory; mirror SQLite's escalation model). */
#define TDB_LOCK_NONE_LEVEL      0
#define TDB_LOCK_SHARED_LEVEL    1
#define TDB_LOCK_RESERVED_LEVEL  2
#define TDB_LOCK_EXCLUSIVE_LEVEL 3

struct tdb_file {
  const tdb_vfs *vfs;
  void          *h;   /* backend-specific handle */
};

struct tdb_vfs {
  const char *name;
  int (*open)(const tdb_vfs *vfs, const char *path, int flags, tdb_file **out);
  int (*read)(tdb_file *f, void *buf, size_t n, uint64_t off);
  int (*write)(tdb_file *f, const void *buf, size_t n, uint64_t off);
  int (*sync)(tdb_file *f);
  int (*truncate)(tdb_file *f, uint64_t size);
  int (*lock)(tdb_file *f, int level);
  int (*filesize)(tdb_file *f, uint64_t *size);
  int (*close)(tdb_file *f);
};

const tdb_vfs *tdb_vfs_default(void);
const tdb_vfs *tdb_vfs_memory(void);

/* Convenience wrappers dispatching through f->vfs. */
int tdb_file_read(tdb_file *f, void *buf, size_t n, uint64_t off);
int tdb_file_write(tdb_file *f, const void *buf, size_t n, uint64_t off);
int tdb_file_sync(tdb_file *f);
int tdb_file_truncate(tdb_file *f, uint64_t size);
int tdb_file_lock(tdb_file *f, int level);
int tdb_file_size(tdb_file *f, uint64_t *size);
int tdb_file_close(tdb_file *f);

#endif /* TDB_FILE_H */
