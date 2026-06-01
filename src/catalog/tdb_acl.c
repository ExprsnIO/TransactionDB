/* tdb_acl.c — access-control list and privilege checks. */
#include "tdb_acl.h"
#include "../common/tdb_mem.h"
#include "../common/tdb_util.h"

#include <string.h>
#include <ctype.h>

static const struct { const char *name; int kind; } k_kinds[] = {
  {"TABLE", TDB_ACL_TABLE}, {"INDEX", TDB_ACL_INDEX},
  {"VIEW", TDB_ACL_VIEW}, {"FUNCTION", TDB_ACL_FUNCTION},
  {"PROCEDURE", TDB_ACL_PROCEDURE},
  {"TABLESPACE", TDB_ACL_TABLESPACE}, {"DATABASE", TDB_ACL_DATABASE},
  {"SEQUENCE", TDB_ACL_SEQUENCE},
};

int tdb_acl_kind_of(const char *s) {
  if (!s) return 0;
  while (*s && isspace((unsigned char)*s)) s++;
  for (size_t i = 0; i < sizeof(k_kinds)/sizeof(k_kinds[0]); i++)
    if (strncasecmp(s, k_kinds[i].name, strlen(k_kinds[i].name)) == 0)
      return k_kinds[i].kind;
  return 0;
}

const char *tdb_acl_kind_name(int kind) {
  for (size_t i = 0; i < sizeof(k_kinds)/sizeof(k_kinds[0]); i++)
    if (k_kinds[i].kind == kind) return k_kinds[i].name;
  return "UNKNOWN";
}

void tdb_acl_init(tdb_acl *a) {
  a->head = NULL; a->count = 0;
}

void tdb_acl_free(tdb_acl *a) {
  tdb_acl_entry *e = a->head;
  while (e) {
    tdb_acl_entry *n = e->next;
    tdb_mfree(e->grantee); tdb_mfree(e->priv); tdb_mfree(e->object);
    tdb_mfree(e);
    e = n;
  }
  a->head = NULL; a->count = 0;
}

static char *up_strdup(const char *s) {
  if (!s) return NULL;
  char *d = tdb_strdup(s);
  if (!d) return NULL;
  for (char *p = d; *p; p++) *p = (char)toupper((unsigned char)*p);
  return d;
}

static int match_grantee(const char *want, const char *user) {
  if (!want || !*want) return 1;
  if (!strcasecmp(want, "PUBLIC")) return 1;
  return user && !strcasecmp(want, user);
}
static int match_priv(const char *want, const char *priv) {
  if (!want || !*want) return 1;
  if (!strcasecmp(want, "ALL")) return 1;
  return priv && !strcasecmp(want, priv);
}
static int match_kind(int want, int kind) {
  return want == 0 || want == kind;
}
static int match_object(const char *want, const char *object) {
  if (!want || !*want) return 1;
  return object && !strcasecmp(want, object);
}

int tdb_acl_check(const tdb_acl *a, const char *user, const char *priv,
                  int kind, const char *object) {
  if (!user) return 1;                  /* superuser / open mode */
  for (const tdb_acl_entry *e = a->head; e; e = e->next) {
    if (match_grantee(e->grantee, user) &&
        match_priv(e->priv, priv) &&
        match_kind(e->kind, kind) &&
        match_object(e->object, object))
      return 1;
  }
  return 0;
}

static int entry_equal(const tdb_acl_entry *a, const tdb_acl_entry *b) {
  return (a->kind == b->kind) &&
         (a->grantee && b->grantee && !strcasecmp(a->grantee, b->grantee)) &&
         (a->priv    && b->priv    && !strcasecmp(a->priv,    b->priv))    &&
         ((a->object == NULL && b->object == NULL) ||
          (a->object && b->object && !strcasecmp(a->object, b->object)));
}

int tdb_acl_grant(tdb_acl *a, const char *grantee, const char *priv,
                  int kind, const char *object) {
  if (!grantee || !priv || !kind) return TDB_MISUSE;
  tdb_acl_entry probe;
  memset(&probe, 0, sizeof(probe));
  probe.grantee = (char *)grantee;
  probe.priv = up_strdup(priv);                    /* canonical case */
  probe.kind = kind;
  probe.object = (char *)object;
  for (tdb_acl_entry *e = a->head; e; e = e->next) {
    if (entry_equal(e, &probe)) { tdb_mfree(probe.priv); return TDB_OK; }
  }
  tdb_acl_entry *ne = (tdb_acl_entry *)tdb_calloc(sizeof(*ne));
  if (!ne) { tdb_mfree(probe.priv); return TDB_NOMEM; }
  ne->grantee = tdb_strdup(grantee);
  ne->priv = probe.priv;                           /* take ownership */
  ne->kind = kind;
  ne->object = object ? tdb_strdup(object) : NULL;
  ne->next = a->head;
  a->head = ne;
  a->count++;
  return TDB_OK;
}

int tdb_acl_revoke(tdb_acl *a, const char *grantee, const char *priv,
                   int kind, const char *object) {
  int removed = 0;
  tdb_acl_entry **pp = &a->head;
  while (*pp) {
    tdb_acl_entry *e = *pp;
    int hit =
        match_grantee(grantee, e->grantee) &&
        match_priv(priv, e->priv) &&
        match_kind(kind, e->kind) &&
        match_object(object, e->object);
    if (hit) {
      *pp = e->next;
      tdb_mfree(e->grantee); tdb_mfree(e->priv); tdb_mfree(e->object);
      tdb_mfree(e);
      removed++;
      a->count--;
    } else pp = &e->next;
  }
  return removed;
}

/* ------------------------------ serialization ------------------------- */

static void w_str(tdb_buf *b, const char *s) {
  if (!s) { tdb_buf_put_varint(b, 0); return; }
  size_t n = strlen(s);
  tdb_buf_put_varint(b, n + 1);
  tdb_buf_append(b, s, n);
}

void tdb_acl_entry_serialize(const tdb_acl_entry *e, tdb_buf *b) {
  tdb_buf_putc(b, 'G');                 /* CAT_GRANT marker */
  uint8_t kind = (uint8_t)e->kind;
  tdb_buf_append(b, &kind, 1);
  w_str(b, e->grantee);
  w_str(b, e->priv);
  w_str(b, e->object);
}

int tdb_acl_entry_deserialize(const uint8_t *p, int n, tdb_acl_entry *out) {
  if (n < 2 || p[0] != 'G') return TDB_CORRUPT;
  memset(out, 0, sizeof(*out));
  int pos = 1;
  out->kind = p[pos++];
  for (int i = 0; i < 3 && pos < n; i++) {
    uint64_t L = 0;
    int a = tdb_get_varint(p + pos, (size_t)(n - pos), &L);
    if (a <= 0) return TDB_CORRUPT;
    pos += a;
    char *s = NULL;
    if (L > 0) {
      int slen = (int)(L - 1);
      if (pos + slen > n) return TDB_CORRUPT;
      s = tdb_strndup((const char *)p + pos, (size_t)slen);
      pos += slen;
    }
    if (i == 0) out->grantee = s;
    else if (i == 1) out->priv = s;
    else out->object = s;
  }
  return TDB_OK;
}
