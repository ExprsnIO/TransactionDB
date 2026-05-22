/*
 * dbfile.cpp — Thin dispatcher between v1 (legacy) and v2 (current).
 *
 * Detects the on-disk version from the file header and routes to the
 * appropriate reader. All writes go through the v2 path; reading a v1 file
 * and saving it produces a v2 file (silent upgrade).
 */

#include "tdb/dbfile.h"
#include <cstdio>
#include <cstring>

namespace tdb::dbfile {

int detect_version(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return 0;
    char magic[4];
    if (fread(magic, 1, 4, f) != 4) { fclose(f); return 0; }
    if (memcmp(magic, "TDB", 3) != 0) { fclose(f); return 0; }
    uint32_t version = 0;
    if (fread(&version, 4, 1, f) != 1) { fclose(f); return 0; }
    fclose(f);
    return (int)version;
}

bool save(const std::string &path, const catalog::Catalog &cat) {
    // Always emit v2.
    return v2::save(path, cat);
}

bool load(const std::string &path, catalog::Catalog &cat) {
    int v = detect_version(path);
    if (v == 2) return v2::load(path, cat);
    if (v == 1) return v1::load(path, cat);
    return false;
}

} // namespace tdb::dbfile
