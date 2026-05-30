/*
** tdb_mem.h — allocation wrappers and a bump (arena) allocator.
**
** The arena is used for parse trees and per-statement scratch: many small
** allocations that share a lifetime and are freed wholesale.
*/
#ifndef TDB_MEM_H
#define TDB_MEM_H

#include "tdb_internal.h"

void *tdb_malloc(size_t n);
void *tdb_calloc(size_t n);
void *tdb_realloc(void *p, size_t n);
void  tdb_mfree(void *p);
char *tdb_strdup(const char *s);
char *tdb_strndup(const char *s, size_t n);

typedef struct tdb_arena tdb_arena;

tdb_arena *tdb_arena_new(size_t block_size);
void      *tdb_arena_alloc(tdb_arena *a, size_t n);
char      *tdb_arena_strndup(tdb_arena *a, const char *s, size_t n);
void       tdb_arena_free(tdb_arena *a);

#endif /* TDB_MEM_H */
