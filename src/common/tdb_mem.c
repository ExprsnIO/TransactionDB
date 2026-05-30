/* tdb_mem.c — allocation wrappers and arena allocator. */
#include "tdb_mem.h"

#include <stdlib.h>
#include <string.h>

void *tdb_malloc(size_t n) { return malloc(n ? n : 1); }
void *tdb_calloc(size_t n) { return calloc(1, n ? n : 1); }
void *tdb_realloc(void *p, size_t n) { return realloc(p, n ? n : 1); }
void  tdb_mfree(void *p) { free(p); }

char *tdb_strdup(const char *s) {
  if (!s) return NULL;
  return tdb_strndup(s, strlen(s));
}

char *tdb_strndup(const char *s, size_t n) {
  char *r = (char *)tdb_malloc(n + 1);
  if (!r) return NULL;
  if (s && n) memcpy(r, s, n);
  r[n] = '\0';
  return r;
}

/* --------------------------- arena allocator --------------------------- */

typedef struct tdb_arena_block {
  struct tdb_arena_block *next;
  size_t used;
  size_t cap;
  /* data follows inline */
} tdb_arena_block;

struct tdb_arena {
  tdb_arena_block *head;
  size_t block_size;
};

#define TDB_ARENA_ALIGN 16
static size_t tdb_align_up(size_t n) {
  return (n + (TDB_ARENA_ALIGN - 1)) & ~(size_t)(TDB_ARENA_ALIGN - 1);
}

static tdb_arena_block *arena_block_new(size_t cap) {
  tdb_arena_block *b =
      (tdb_arena_block *)tdb_malloc(sizeof(tdb_arena_block) + cap);
  if (!b) return NULL;
  b->next = NULL;
  b->used = 0;
  b->cap = cap;
  return b;
}

tdb_arena *tdb_arena_new(size_t block_size) {
  tdb_arena *a = (tdb_arena *)tdb_malloc(sizeof(*a));
  if (!a) return NULL;
  if (block_size < 4096) block_size = 4096;
  a->block_size = block_size;
  a->head = arena_block_new(block_size);
  if (!a->head) {
    tdb_mfree(a);
    return NULL;
  }
  return a;
}

void *tdb_arena_alloc(tdb_arena *a, size_t n) {
  if (!a) return NULL;
  n = tdb_align_up(n ? n : 1);
  tdb_arena_block *b = a->head;
  if (b->used + n > b->cap) {
    /* allocate a dedicated block large enough for oversized requests */
    size_t cap = n > a->block_size ? n : a->block_size;
    tdb_arena_block *nb = arena_block_new(cap);
    if (!nb) return NULL;
    nb->next = a->head;
    a->head = nb;
    b = nb;
  }
  void *p = (char *)(b + 1) + b->used;
  b->used += n;
  return p;
}

char *tdb_arena_strndup(tdb_arena *a, const char *s, size_t n) {
  char *r = (char *)tdb_arena_alloc(a, n + 1);
  if (!r) return NULL;
  if (s && n) memcpy(r, s, n);
  r[n] = '\0';
  return r;
}

void tdb_arena_free(tdb_arena *a) {
  if (!a) return;
  tdb_arena_block *b = a->head;
  while (b) {
    tdb_arena_block *next = b->next;
    tdb_mfree(b);
    b = next;
  }
  tdb_mfree(a);
}
