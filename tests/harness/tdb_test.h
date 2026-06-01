// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rick Holland
//
// Minimal header-only test harness. Zero dependencies, single TU per test.
// Each test binary defines one main() that runs a flat list of TDB_TEST blocks.
// CTest treats each binary as one test; failures print file:line and abort the
// binary with a non-zero exit code so CTest marks it failed.

#ifndef TDB_TEST_H
#define TDB_TEST_H

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <functional>

namespace tdb_test {

struct Case {
    const char *name;
    std::function<void()> fn;
};

inline std::vector<Case> &registry() {
    static std::vector<Case> r;
    return r;
}

struct Registrar {
    Registrar(const char *name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

inline int run_all(const char *suite) {
    int failed = 0;
    int total  = 0;
    for (auto &c : registry()) {
        ++total;
        std::fprintf(stdout, "[ RUN  ] %s.%s\n", suite, c.name);
        try {
            c.fn();
            std::fprintf(stdout, "[  OK  ] %s.%s\n", suite, c.name);
        } catch (const std::exception &e) {
            std::fprintf(stdout, "[ FAIL ] %s.%s : %s\n", suite, c.name, e.what());
            ++failed;
        } catch (...) {
            std::fprintf(stdout, "[ FAIL ] %s.%s : unknown\n", suite, c.name);
            ++failed;
        }
    }
    std::fprintf(stdout, "%s: %d/%d passed\n", suite, total - failed, total);
    return failed == 0 ? 0 : 1;
}

} // namespace tdb_test

#define TDB_TEST(name)                                                         \
    static void tdb_test_##name();                                             \
    static ::tdb_test::Registrar tdb_test_reg_##name(#name, tdb_test_##name);  \
    static void tdb_test_##name()

#define TDB_REQUIRE(cond)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            char buf[512];                                                     \
            std::snprintf(buf, sizeof(buf),                                    \
                          "%s:%d: TDB_REQUIRE(%s) failed",                     \
                          __FILE__, __LINE__, #cond);                          \
            throw std::runtime_error(buf);                                     \
        }                                                                      \
    } while (0)

#define TDB_REQUIRE_EQ(a, b)                                                   \
    do {                                                                       \
        auto _a = (a);                                                         \
        auto _b = (b);                                                         \
        if (!(_a == _b)) {                                                     \
            char buf[512];                                                     \
            std::snprintf(buf, sizeof(buf),                                    \
                          "%s:%d: TDB_REQUIRE_EQ(%s, %s) failed",              \
                          __FILE__, __LINE__, #a, #b);                         \
            throw std::runtime_error(buf);                                     \
        }                                                                      \
    } while (0)

#define TDB_TEST_MAIN(suite)                                                   \
    int main(int, char **) { return ::tdb_test::run_all(suite); }

#endif
