/*
** tdb_test.h — a tiny, dependency-free unit-test harness.
**
** Each test file defines test functions and lists them in a TDB_TESTS table,
** then calls TDB_MAIN(). Failures print file:line and the test continues to
** the next case; the process exit code is non-zero if any assertion failed.
*/
#ifndef TDB_TEST_H
#define TDB_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tdb__fail_count = 0;
static int tdb__check_count = 0;

#define TDB_CHECK(cond)                                                    \
  do {                                                                     \
    tdb__check_count++;                                                    \
    if (!(cond)) {                                                         \
      tdb__fail_count++;                                                   \
      fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
    }                                                                      \
  } while (0)

#define TDB_CHECK_EQ(a, b)                                                 \
  do {                                                                     \
    tdb__check_count++;                                                    \
    long long _va = (long long)(a), _vb = (long long)(b);                  \
    if (_va != _vb) {                                                      \
      tdb__fail_count++;                                                   \
      fprintf(stderr, "  FAIL %s:%d: %s == %s (got %lld vs %lld)\n",       \
              __FILE__, __LINE__, #a, #b, _va, _vb);                       \
    }                                                                      \
  } while (0)

#define TDB_CHECK_STR(a, b)                                                \
  do {                                                                     \
    tdb__check_count++;                                                    \
    const char *_sa = (a), *_sb = (b);                                     \
    if (!_sa || !_sb || strcmp(_sa, _sb) != 0) {                          \
      tdb__fail_count++;                                                   \
      fprintf(stderr, "  FAIL %s:%d: \"%s\" == \"%s\"\n",                  \
              __FILE__, __LINE__, _sa ? _sa : "(null)",                    \
              _sb ? _sb : "(null)");                                       \
    }                                                                      \
  } while (0)

typedef void (*tdb_test_fn)(void);
typedef struct { const char *name; tdb_test_fn fn; } tdb_test_case;

#define TDB_RUN_ALL(cases)                                                 \
  do {                                                                     \
    size_t _n = sizeof(cases) / sizeof((cases)[0]);                        \
    for (size_t _i = 0; _i < _n; _i++) {                                   \
      int before = tdb__fail_count;                                        \
      fprintf(stderr, "[ RUN  ] %s\n", (cases)[_i].name);                  \
      (cases)[_i].fn();                                                    \
      fprintf(stderr, "[ %s ] %s\n",                                       \
              tdb__fail_count == before ? "PASS" : "FAIL",                 \
              (cases)[_i].name);                                           \
    }                                                                      \
    fprintf(stderr, "%d checks, %d failures\n",                            \
            tdb__check_count, tdb__fail_count);                            \
  } while (0)

#define TDB_MAIN(cases)                                                    \
  int main(void) {                                                         \
    TDB_RUN_ALL(cases);                                                    \
    return tdb__fail_count == 0 ? 0 : 1;                                   \
  }

#endif /* TDB_TEST_H */
