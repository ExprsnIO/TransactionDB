/*
** tdb_acl.h — access-control list (GRANT / REVOKE).
**
** Each entry pairs a grantee (role / user name, or "PUBLIC") with a
** privilege string ("SELECT", "INSERT", "UPDATE", "DELETE", "EXECUTE",
** "USAGE", "ALL") and an object reference (kind + name). Entries are
** persisted in the catalog b-tree under a dedicated record type so
** GRANTs survive process restarts.
**
** Policy
**   1. If the connection has no current user set (`db->current_user == NULL`)
**      the database is in "open" mode: all checks pass. This matches the
**      pre-ACL behaviour of TransactionDB.
**   2. Otherwise the check returns TDB_OK if (and only if) some ACL entry
**      matches the (user-or-PUBLIC, priv-or-ALL, kind, name) tuple.
*/
#ifndef TDB_ACL_H
#define TDB_ACL_H

#include "../common/tdb_internal.h"
#include "../common/tdb_buf.h"

/* Object kinds the ACL knows about. */
#define TDB_ACL_TABLE        1
#define TDB_ACL_INDEX        2
#define TDB_ACL_VIEW         3
#define TDB_ACL_FUNCTION     4
#define TDB_ACL_PROCEDURE    5
#define TDB_ACL_TABLESPACE   6
#define TDB_ACL_DATABASE     7
#define TDB_ACL_SEQUENCE     8

typedef struct tdb_acl_entry {
  char *grantee;     /* user or role name, or "PUBLIC" */
  char *priv;        /* canonical uppercase privilege keyword */
  int   kind;        /* TDB_ACL_* */
  char *object;      /* object name (or NULL for ANY) */
  struct tdb_acl_entry *next;
} tdb_acl_entry;

typedef struct tdb_acl {
  tdb_acl_entry *head;
  int            count;
} tdb_acl;

/* Resolve a TDB_ACL_* kind from a textual hint ("TABLE", "INDEX", ...).
** Returns 0 if `s` is empty/NULL and matches nothing. */
int  tdb_acl_kind_of(const char *s);
const char *tdb_acl_kind_name(int kind);

/* Lifecycle. */
void tdb_acl_init(tdb_acl *a);
void tdb_acl_free(tdb_acl *a);

/* Insert one entry (copies of the strings; transfers no ownership).
** Inserting a duplicate is a no-op. */
int  tdb_acl_grant(tdb_acl *a, const char *grantee, const char *priv,
                   int kind, const char *object);

/* Remove matching entries. NULL params match any. Returns the # removed. */
int  tdb_acl_revoke(tdb_acl *a, const char *grantee, const char *priv,
                    int kind, const char *object);

/* Returns non-zero if a matching entry exists, treating "ALL" privileges
** and a NULL/empty object as wildcards. */
int  tdb_acl_check(const tdb_acl *a, const char *user, const char *priv,
                   int kind, const char *object);

/* Serialize one entry into a buffer (for catalog persistence). */
void tdb_acl_entry_serialize(const tdb_acl_entry *e, tdb_buf *b);
int  tdb_acl_entry_deserialize(const uint8_t *p, int n, tdb_acl_entry *out);

#endif /* TDB_ACL_H */
