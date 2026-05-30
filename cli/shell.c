/*
** shell.c — TransactionDB interactive shell (REPL).
**
** Reads SQL terminated by ';' and prints result tables, plus a handful of
** dot-commands. Being a first-party tool, it reaches into a couple of internal
** headers to implement schema introspection (.tables / .schema).
*/
#include "transactiondb.h"
#include "tdb_db.h"               /* internal: struct tdb_db */
#include "tdb_catalog.h"          /* internal: catalog introspection */
#include "tdb_sqltype.h"          /* internal: type-id names */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_result(tdb_stmt *st, int first_was_row) {
  int nc = tdb_column_count(st);
  if (nc <= 0) return;
  for (int i = 0; i < nc; i++) {
    const char *n = tdb_column_name(st, i);
    printf("%s%s", i ? " | " : "", n ? n : "?");
  }
  printf("\n");
  for (int i = 0; i < nc; i++) printf("%s---", i ? "-+-" : "");
  printf("\n");

  int have = first_was_row;
  while (have) {
    for (int i = 0; i < nc; i++) {
      const char *t = tdb_column_text(st, i);
      printf("%s%s", i ? " | " : "", t ? t : "NULL");
    }
    printf("\n");
    have = (tdb_step(st) == TDB_ROW);
  }
}

static void run_sql(tdb_db *db, const char *sql) {
  const char *tail = sql;
  while (tail && *tail) {
    tdb_stmt *st = NULL;
    const char *next = NULL;
    int rc = tdb_prepare_v2(db, tail, -1, &st, &next);
    if (rc != TDB_OK) { fprintf(stderr, "Error: %s\n", tdb_errmsg(db)); return; }
    if (!st) { tail = next; continue; }

    int s = tdb_step(st);
    if (s == TDB_ROW || (s == TDB_DONE && tdb_column_count(st) > 0)) {
      print_result(st, s == TDB_ROW);
    } else if (s != TDB_DONE) {
      fprintf(stderr, "Error: %s\n", tdb_errmsg(db));
    }
    tdb_finalize(st);
    tail = next;
  }
}

static void cmd_tables(tdb_db *db) {
  int n = tdb_catalog_table_count(db->cat);
  for (int i = 0; i < n; i++) printf("%s\n", tdb_catalog_table_at(db->cat, i)->name);
}

static void cmd_schema(tdb_db *db, const char *want) {
  int n = tdb_catalog_table_count(db->cat);
  for (int i = 0; i < n; i++) {
    tdb_table *t = tdb_catalog_table_at(db->cat, i);
    if (want && *want && strcasecmp(want, t->name) != 0) continue;
    printf("TABLE %s\n", t->name);
    for (int c = 0; c < t->ncol; c++) {
      tdb_column *col = &t->cols[c];
      printf("  %-20s %s%s%s%s\n", col->name, tdb_typeid_name(col->type.id),
             col->pk ? " PRIMARY KEY" : "", col->notnull ? " NOT NULL" : "",
             col->generated != TDB_GEN_NONE ? " GENERATED" : "");
    }
  }
}

static void run_file(tdb_db *db, const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "cannot open %s\n", path); return; }
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  char *buf = (char *)malloc((size_t)sz + 1);
  if (buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz) { buf[sz] = '\0'; run_sql(db, buf); }
  free(buf);
  fclose(f);
}

static int handle_dot(tdb_db **db, const char *line, const char *initial_path) {
  char cmd[64] = {0}, arg[1024] = {0};
  sscanf(line, ".%63s %1023[^\n]", cmd, arg);
  if (!strcmp(cmd, "quit") || !strcmp(cmd, "exit")) return 1;
  if (!strcmp(cmd, "help")) {
    printf(".tables            list tables\n"
           ".schema [TABLE]    show schema\n"
           ".read FILE         execute SQL from FILE\n"
           ".open FILE         open a different database\n"
           ".quit / .exit      leave the shell\n");
  } else if (!strcmp(cmd, "tables")) {
    cmd_tables(*db);
  } else if (!strcmp(cmd, "schema")) {
    cmd_schema(*db, arg);
  } else if (!strcmp(cmd, "read")) {
    run_file(*db, arg);
  } else if (!strcmp(cmd, "open")) {
    tdb_db *nd = NULL;
    if (tdb_open(arg[0] ? arg : initial_path, &nd) == TDB_OK) { tdb_close(*db); *db = nd; }
    else fprintf(stderr, "cannot open %s\n", arg);
  } else {
    fprintf(stderr, "unknown command: .%s\n", cmd);
  }
  return 0;
}

/* does the trimmed buffer end with a ';'? */
static int complete(const char *buf) {
  size_t n = strlen(buf);
  while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' ' || buf[n - 1] == '\t' || buf[n - 1] == '\r')) n--;
  return n > 0 && buf[n - 1] == ';';
}

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : ":memory:";
  printf("TransactionDB shell %s  (\".help\" for commands)\n", tdb_libversion());

  tdb_db *db = NULL;
  if (tdb_open(path, &db) != TDB_OK) {
    fprintf(stderr, "cannot open %s\n", path);
    return 1;
  }
  printf("Connected to %s\n", path);

  char line[4096];
  char buf[1 << 16];
  buf[0] = '\0';
  int in_stmt = 0;

  for (;;) {
    printf("%s", in_stmt ? "  ...> " : "tdb> ");
    fflush(stdout);
    if (!fgets(line, sizeof(line), stdin)) break;

    if (!in_stmt && line[0] == '.') {
      if (handle_dot(&db, line, path)) break;
      continue;
    }
    if (strlen(buf) + strlen(line) + 1 >= sizeof(buf)) { fprintf(stderr, "statement too long\n"); buf[0] = '\0'; in_stmt = 0; continue; }
    strcat(buf, line);
    in_stmt = 1;
    if (complete(buf)) { run_sql(db, buf); buf[0] = '\0'; in_stmt = 0; }
  }

  tdb_close(db);
  printf("\n");
  return 0;
}
