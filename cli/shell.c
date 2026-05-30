/*
** shell.c — TransactionDB interactive shell (REPL).
**
** Phase-0 form: prints a banner and echoes that SQL execution is not yet
** wired. The full REPL (dot-commands, prepared-statement result tables) is
** implemented in Phase 8.
*/
#include "transactiondb.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : ":memory:";
  printf("TransactionDB shell %s\n", tdb_libversion());
  printf("Opening database: %s\n", path);

  tdb_db *db = NULL;
  int rc = tdb_open(path, &db);
  if (rc != TDB_OK) {
    fprintf(stderr, "cannot open database: %s\n", tdb_status_str(rc));
    return 1;
  }

  printf("Connected. (SQL execution arrives in a later build phase.)\n");
  printf("Type .quit to exit.\n");

  char line[4096];
  while (printf("tdb> "), fflush(stdout), fgets(line, sizeof(line), stdin)) {
    if (strncmp(line, ".quit", 5) == 0 || strncmp(line, ".exit", 5) == 0) break;
    if (line[0] == '\n') continue;
    if (strncmp(line, ".help", 5) == 0) {
      printf(".quit / .exit   exit the shell\n.help           this message\n");
      continue;
    }
    printf("(not yet executed) %s", line);
  }

  tdb_close(db);
  printf("\nGoodbye.\n");
  return 0;
}
