/*
** tdb_catalog.h — the system catalog (schema persistence).
**
** Schema objects (tables, views, routines) are stored in a dedicated catalog
** b-tree — the analogue of sqlite_master — whose root page is recorded in the
** database header. Each object is kept as one row holding a compact binary
** serialization of its in-memory structure. On open the catalog b-tree is
** scanned and every object is rebuilt into an in-memory cache for fast lookup.
**
** (Until the SQL parser lands in Phase 6, DDL is expressed by building the
** schema structs directly and handing them to tdb_catalog_add_*; binary
** serialization avoids needing to round-trip through SQL text.)
*/
#ifndef TDB_CATALOG_H
#define TDB_CATALOG_H

#include "tdb_schema.h"
#include "../storage/tdb_pager.h"

typedef struct tdb_catalog tdb_catalog;

/* Open the catalog, creating and persisting the catalog b-tree on a fresh
** database, then load all objects into memory. */
int  tdb_catalog_open(tdb_pager *p, tdb_catalog **out);
void tdb_catalog_close(tdb_catalog *c);

/* Persist a new object and adopt ownership of it. Requires an active pager
** write transaction (the caller commits). */
int  tdb_catalog_add_table(tdb_catalog *c, tdb_table *t);
int  tdb_catalog_add_view(tdb_catalog *c, tdb_view *v);
int  tdb_catalog_add_routine(tdb_catalog *c, tdb_routine *r);

tdb_table   *tdb_catalog_find_table(tdb_catalog *c, const char *name);
tdb_view    *tdb_catalog_find_view(tdb_catalog *c, const char *name);
tdb_routine *tdb_catalog_find_routine(tdb_catalog *c, const char *name);

int          tdb_catalog_table_count(tdb_catalog *c);
tdb_table   *tdb_catalog_table_at(tdb_catalog *c, int i);
int          tdb_catalog_routine_count(tdb_catalog *c);
tdb_routine *tdb_catalog_routine_at(tdb_catalog *c, int i);

/* Remove a table from the in-memory cache (session-scoped; on-disk catalog
** row reclamation is left to a future vacuum/rewrite). */
void       tdb_catalog_drop_table(tdb_catalog *c, const char *name);
void       tdb_catalog_drop_view(tdb_catalog *c, const char *name);

/* Rewrite a table's persisted catalog row (e.g. after CREATE INDEX adds an
** index to an existing table). The *_as variant locates the row by a given
** name (used by ALTER TABLE ... RENAME TO). */
int        tdb_catalog_update_table(tdb_catalog *c, tdb_table *t);
int        tdb_catalog_update_table_as(tdb_catalog *c, const char *find_name, tdb_table *t);

#endif /* TDB_CATALOG_H */
