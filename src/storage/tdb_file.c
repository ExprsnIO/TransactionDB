/* tdb_file.c — default OS-backed VFS and an in-memory VFS. */
#include "tdb_file.h"
#include "../common/tdb_mem.h"

#include <string.h>
#include <stdio.h>

/* Dispatch wrappers ---------------------------------------------------- */
int tdb_file_read(tdb_file *f, void *buf, size_t n, uint64_t off) {
  return f->vfs->read(f, buf, n, off);
}
int tdb_file_write(tdb_file *f, const void *buf, size_t n, uint64_t off) {
  return f->vfs->write(f, buf, n, off);
}
int tdb_file_sync(tdb_file *f) { return f->vfs->sync(f); }
int tdb_file_truncate(tdb_file *f, uint64_t size) { return f->vfs->truncate(f, size); }
int tdb_file_lock(tdb_file *f, int level) { return f->vfs->lock(f, level); }
int tdb_file_size(tdb_file *f, uint64_t *size) { return f->vfs->filesize(f, size); }
int tdb_file_close(tdb_file *f) { return f ? f->vfs->close(f) : TDB_OK; }

/* ===================================================================== */
/* Default OS-backed VFS (POSIX / stdio fallback)                        */
/* ===================================================================== */
#if defined(_WIN32)
  #define TDB_USE_STDIO 1
#else
  #include <fcntl.h>
  #include <unistd.h>
  #include <sys/stat.h>
  #include <errno.h>
#endif

#if !defined(TDB_USE_STDIO)

typedef struct { int fd; } os_handle;

static int os_open(const tdb_vfs *vfs, const char *path, int flags,
                   tdb_file **out) {
  int oflags = 0;
  if (flags & TDB_OPEN_READONLY) {
    oflags = O_RDONLY;
  } else {
    oflags = O_RDWR;
    if (flags & TDB_OPEN_CREATE) oflags |= O_CREAT;
  }
  int fd = open(path, oflags, 0644);
  if (fd < 0) return (errno == ENOENT) ? TDB_NOTFOUND : TDB_IOERR;

  tdb_file *f = (tdb_file *)tdb_calloc(sizeof(*f));
  os_handle *h = (os_handle *)tdb_malloc(sizeof(*h));
  if (!f || !h) { close(fd); tdb_mfree(f); tdb_mfree(h); return TDB_NOMEM; }
  h->fd = fd;
  f->vfs = vfs;
  f->h = h;
  *out = f;
  return TDB_OK;
}

static int os_read(tdb_file *f, void *buf, size_t n, uint64_t off) {
  os_handle *h = (os_handle *)f->h;
  size_t done = 0;
  while (done < n) {
    ssize_t r = pread(h->fd, (char *)buf + done, n - done, (off_t)(off + done));
    if (r < 0) { if (errno == EINTR) continue; return TDB_IOERR; }
    if (r == 0) { /* short read: zero-fill the tail (sparse/new pages) */
      memset((char *)buf + done, 0, n - done);
      break;
    }
    done += (size_t)r;
  }
  return TDB_OK;
}

static int os_write(tdb_file *f, const void *buf, size_t n, uint64_t off) {
  os_handle *h = (os_handle *)f->h;
  size_t done = 0;
  while (done < n) {
    ssize_t w = pwrite(h->fd, (const char *)buf + done, n - done,
                       (off_t)(off + done));
    if (w < 0) { if (errno == EINTR) continue; return TDB_IOERR; }
    done += (size_t)w;
  }
  return TDB_OK;
}

static int os_sync(tdb_file *f) {
  os_handle *h = (os_handle *)f->h;
#if defined(__APPLE__)
  return fcntl(h->fd, F_FULLFSYNC) == 0 ? TDB_OK : TDB_IOERR;
#else
  return fsync(h->fd) == 0 ? TDB_OK : TDB_IOERR;
#endif
}

static int os_truncate(tdb_file *f, uint64_t size) {
  os_handle *h = (os_handle *)f->h;
  return ftruncate(h->fd, (off_t)size) == 0 ? TDB_OK : TDB_IOERR;
}

static int os_lock(tdb_file *f, int level) {
  os_handle *h = (os_handle *)f->h;
  struct flock fl;
  memset(&fl, 0, sizeof(fl));
  fl.l_whence = SEEK_SET;
  fl.l_start = 0;
  fl.l_len = 0; /* whole file */
  fl.l_type = (level == TDB_LOCK_NONE_LEVEL) ? F_UNLCK
            : (level >= TDB_LOCK_RESERVED_LEVEL) ? F_WRLCK : F_RDLCK;
  if (fcntl(h->fd, F_SETLK, &fl) != 0) return TDB_BUSY;
  return TDB_OK;
}

static int os_filesize(tdb_file *f, uint64_t *size) {
  os_handle *h = (os_handle *)f->h;
  struct stat st;
  if (fstat(h->fd, &st) != 0) return TDB_IOERR;
  *size = (uint64_t)st.st_size;
  return TDB_OK;
}

static int os_close(tdb_file *f) {
  os_handle *h = (os_handle *)f->h;
  if (h) { close(h->fd); tdb_mfree(h); }
  tdb_mfree(f);
  return TDB_OK;
}

static const tdb_vfs g_default_vfs = {
  "unix", os_open, os_read, os_write, os_sync,
  os_truncate, os_lock, os_filesize, os_close};

const tdb_vfs *tdb_vfs_default(void) { return &g_default_vfs; }

#else /* TDB_USE_STDIO — portable fallback (Windows, etc.) */

typedef struct { FILE *fp; } os_handle;

static int os_open(const tdb_vfs *vfs, const char *path, int flags,
                   tdb_file **out) {
  const char *mode = (flags & TDB_OPEN_READONLY) ? "rb" : "r+b";
  FILE *fp = fopen(path, mode);
  if (!fp && (flags & TDB_OPEN_CREATE)) fp = fopen(path, "w+b");
  if (!fp) return TDB_NOTFOUND;
  tdb_file *f = (tdb_file *)tdb_calloc(sizeof(*f));
  os_handle *h = (os_handle *)tdb_malloc(sizeof(*h));
  if (!f || !h) { fclose(fp); tdb_mfree(f); tdb_mfree(h); return TDB_NOMEM; }
  h->fp = fp; f->vfs = vfs; f->h = h; *out = f;
  return TDB_OK;
}
static int os_read(tdb_file *f, void *buf, size_t n, uint64_t off) {
  os_handle *h = (os_handle *)f->h;
  if (fseek(h->fp, (long)off, SEEK_SET) != 0) return TDB_IOERR;
  size_t r = fread(buf, 1, n, h->fp);
  if (r < n) memset((char *)buf + r, 0, n - r);
  return TDB_OK;
}
static int os_write(tdb_file *f, const void *buf, size_t n, uint64_t off) {
  os_handle *h = (os_handle *)f->h;
  if (fseek(h->fp, (long)off, SEEK_SET) != 0) return TDB_IOERR;
  return fwrite(buf, 1, n, h->fp) == n ? TDB_OK : TDB_IOERR;
}
static int os_sync(tdb_file *f) { os_handle *h=(os_handle*)f->h; return fflush(h->fp)==0?TDB_OK:TDB_IOERR; }
static int os_truncate(tdb_file *f, uint64_t size) { (void)f;(void)size; return TDB_OK; }
static int os_lock(tdb_file *f, int level) { (void)f;(void)level; return TDB_OK; }
static int os_filesize(tdb_file *f, uint64_t *size) {
  os_handle *h=(os_handle*)f->h;
  if (fseek(h->fp,0,SEEK_END)!=0) return TDB_IOERR;
  *size=(uint64_t)ftell(h->fp); return TDB_OK;
}
static int os_close(tdb_file *f) { os_handle *h=(os_handle*)f->h; if(h){fclose(h->fp);tdb_mfree(h);} tdb_mfree(f); return TDB_OK; }

static const tdb_vfs g_default_vfs = {
  "stdio", os_open, os_read, os_write, os_sync,
  os_truncate, os_lock, os_filesize, os_close};
const tdb_vfs *tdb_vfs_default(void) { return &g_default_vfs; }

#endif

/* ===================================================================== */
/* In-memory VFS (for tests / :memory: databases)                        */
/* ===================================================================== */
typedef struct {
  uint8_t *data;
  uint64_t size;
  uint64_t cap;
} mem_handle;

static int mem_open(const tdb_vfs *vfs, const char *path, int flags,
                    tdb_file **out) {
  TDB_UNUSED(path); TDB_UNUSED(flags);
  tdb_file *f = (tdb_file *)tdb_calloc(sizeof(*f));
  mem_handle *h = (mem_handle *)tdb_calloc(sizeof(*h));
  if (!f || !h) { tdb_mfree(f); tdb_mfree(h); return TDB_NOMEM; }
  f->vfs = vfs;
  f->h = h;
  *out = f;
  return TDB_OK;
}

static int mem_grow(mem_handle *h, uint64_t need) {
  if (need <= h->cap) return TDB_OK;
  uint64_t cap = h->cap ? h->cap : 4096;
  while (cap < need) cap *= 2;
  uint8_t *p = (uint8_t *)tdb_realloc(h->data, (size_t)cap);
  if (!p) return TDB_NOMEM;
  memset(p + h->cap, 0, (size_t)(cap - h->cap));
  h->data = p;
  h->cap = cap;
  return TDB_OK;
}

static int mem_read(tdb_file *f, void *buf, size_t n, uint64_t off) {
  mem_handle *h = (mem_handle *)f->h;
  if (off >= h->size) { memset(buf, 0, n); return TDB_OK; }
  uint64_t avail = h->size - off;
  size_t copy = avail < n ? (size_t)avail : n;
  memcpy(buf, h->data + off, copy);
  if (copy < n) memset((char *)buf + copy, 0, n - copy);
  return TDB_OK;
}

static int mem_write(tdb_file *f, const void *buf, size_t n, uint64_t off) {
  mem_handle *h = (mem_handle *)f->h;
  int rc = mem_grow(h, off + n);
  if (rc) return rc;
  memcpy(h->data + off, buf, n);
  if (off + n > h->size) h->size = off + n;
  return TDB_OK;
}

static int mem_sync(tdb_file *f) { TDB_UNUSED(f); return TDB_OK; }

static int mem_truncate(tdb_file *f, uint64_t size) {
  mem_handle *h = (mem_handle *)f->h;
  if (size < h->size) h->size = size;
  return TDB_OK;
}

static int mem_lock(tdb_file *f, int level) { TDB_UNUSED(f); TDB_UNUSED(level); return TDB_OK; }

static int mem_filesize(tdb_file *f, uint64_t *size) {
  mem_handle *h = (mem_handle *)f->h;
  *size = h->size;
  return TDB_OK;
}

static int mem_close(tdb_file *f) {
  mem_handle *h = (mem_handle *)f->h;
  if (h) { tdb_mfree(h->data); tdb_mfree(h); }
  tdb_mfree(f);
  return TDB_OK;
}

static const tdb_vfs g_memory_vfs = {
  "memory", mem_open, mem_read, mem_write, mem_sync,
  mem_truncate, mem_lock, mem_filesize, mem_close};

const tdb_vfs *tdb_vfs_memory(void) { return &g_memory_vfs; }
