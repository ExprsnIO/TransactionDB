#ifndef TDB_VERSION_OBJECT_H
#define TDB_VERSION_OBJECT_H

#include <cstdint>
#include <string>
#include <chrono>

namespace tdb {

// SemVer-style version stamp carried by every catalog object.
struct ObjectVersion {
    uint8_t  major = 1;
    uint8_t  minor = 0;
    uint8_t  patch = 0;
    uint64_t created_epoch_ms = 0;

    static uint64_t now_ms() {
        using namespace std::chrono;
        return (uint64_t)duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()).count();
    }

    static ObjectVersion initial() {
        return ObjectVersion{1, 0, 0, now_ms()};
    }

    std::string to_string() const {
        return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    }

    bool equals(const ObjectVersion &o) const {
        return major == o.major && minor == o.minor && patch == o.patch;
    }

    // Lex order (treat as major.minor.patch numeric tuple).
    bool less_than(const ObjectVersion &o) const {
        if (major != o.major) return major < o.major;
        if (minor != o.minor) return minor < o.minor;
        return patch < o.patch;
    }
};

enum class BumpKind { Breaking, Additive, Fix };

inline ObjectVersion bump(const ObjectVersion &v, BumpKind kind) {
    ObjectVersion nv = v;
    nv.created_epoch_ms = ObjectVersion::now_ms();
    switch (kind) {
        case BumpKind::Breaking: nv.major++; nv.minor = 0; nv.patch = 0; break;
        case BumpKind::Additive: nv.minor++; nv.patch = 0; break;
        case BumpKind::Fix:      nv.patch++; break;
    }
    return nv;
}

// Append-only history entry. `serialized_snapshot` is bincode of the prior
// Info struct (kind-specific format owned by the dbfile layer).
struct VersionHistoryEntry {
    std::string  object_kind;     // "table" | "view" | "document" | "script" | ...
    std::string  object_name;
    ObjectVersion version;
    uint64_t     timestamp_ms = 0;
    std::string  change_summary;
    std::string  serialized_snapshot;
};

} // namespace tdb

#endif // TDB_VERSION_OBJECT_H
