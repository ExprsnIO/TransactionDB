# TransactionDB.framework (Apple)

This directory packages TransactionDB as a native Apple **framework** that vends
both the **C API** (`transactiondb.h`) and the **C++17 RAII wrapper**
(`transactiondb.hpp`) to **C, Objective-C, Objective-C++, and Swift** clients.

| Client | Imports | Gets |
| --- | --- | --- |
| C / Objective-C | `#import <TransactionDB/TransactionDB.h>` | C API (`tdb_*`) |
| Objective-C++ | `#import <TransactionDB/transactiondb.hpp>` | C++ API (`tdb::*`) + C API |
| Swift | `import TransactionDB` | C API (`tdb_*`) |
| Swift + C++ interop | `import TransactionDB` | C++ API (`tdb::*`) + C API |

## How it works

The framework ships a hand-written [`module.modulemap`](module.modulemap):

- The **top-level module** exposes the C API via the umbrella header
  [`TransactionDB.h`](TransactionDB.h). Every client language can `import` /
  `#import` it.
- An **explicit `CXX` submodule** carries `transactiondb.hpp` and is guarded by
  `requires cplusplus`. It therefore only resolves for C++ / Objective-C++ and
  for Swift compiled with C++ interoperability — pure C / Objective-C / Swift
  builds never try to parse the C++ header.

Bundle layout produced by the build:

```
TransactionDB.framework/
├── Headers/
│   ├── TransactionDB.h        # umbrella (C API)
│   ├── transactiondb.h        # C API
│   └── transactiondb.hpp      # C++ RAII wrapper
├── Modules/
│   └── module.modulemap
├── Resources/Info.plist
└── TransactionDB              # the dylib
```

## Building a single .framework

The framework is produced by the main CMake build, gated behind
`-DTDB_BUILD_FRAMEWORK=ON` (Apple platforms only; it is a no-op elsewhere). It
forces a shared library, sets the bundle's `Info.plist`, copies the public
headers into `Headers/`, and installs the module map into `Modules/`.

```sh
# macOS (universal):
cmake -S . -B build-mac -G Xcode \
  -DTDB_BUILD_FRAMEWORK=ON -DTDB_BUILD_TESTS=OFF -DTDB_BUILD_CLI=OFF -DTDB_BUILD_LUA=OFF \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build build-mac --config Release
# -> build-mac/Release/TransactionDB.framework
```

> The Ninja/Makefile generators also work, but the **Xcode** generator is
> recommended for framework + module emission, and is what the xcframework
> script below uses.

## Building a multi-platform .xcframework

[`build-xcframework.sh`](build-xcframework.sh) builds the framework for macOS,
iOS device, and iOS simulator, then bundles them with
`xcodebuild -create-xcframework`:

```sh
apple/build-xcframework.sh
# -> dist/TransactionDB.xcframework
```

Useful overrides (environment variables): `CONFIG`, `IOS_DEPLOYMENT_TARGET`,
`MACOS_DEPLOYMENT_TARGET`, `TDB_BUILD_LUA` (off by default — Lua is fetched over
the network and is awkward to cross-compile for iOS).

Requires macOS with Xcode and CMake ≥ 3.16.

## Consuming it

### Drag-and-drop (Xcode)

Add `TransactionDB.xcframework` to your target's *Frameworks, Libraries, and
Embedded Content* (Embed & Sign). Then import as shown above.

### Swift Package Manager (binary target)

```swift
.binaryTarget(
    name: "TransactionDB",
    path: "dist/TransactionDB.xcframework"
)
```

To use the **C++** API from Swift, enable interop on the consuming target:

```swift
.target(
    name: "App",
    dependencies: ["TransactionDB"],
    swiftSettings: [.interoperabilityMode(.Cxx)]
)
```

In Xcode the equivalent build setting is **C++ and Objective-C
Interoperability → C++ / Objective-C++** (`SWIFT_OBJC_INTEROP_MODE = objcxx`).

## API at a glance

The framework simply re-exports the engine's existing public headers; nothing
about the API changes when consumed through the framework.

- **C** (`transactiondb.h`): `tdb_open` / `tdb_prepare_v2` / `tdb_step` /
  `tdb_bind_*` / `tdb_column_*` / `tdb_finalize` / `tdb_close`. Bind indices are
  1-based; column indices are 0-based; `tdb_step` returns `TDB_ROW` / `TDB_DONE`.
- **C++** (`transactiondb.hpp`): `tdb::Database` and `tdb::Statement`
  (move-only, RAII). `db.exec(sql)`, `db.prepare(sql)`, `stmt.bind(i, v)`,
  `stmt.step()`, `stmt.getInt(i)` / `getDouble(i)` / `getText(i)`. Errors throw
  `tdb::Error`.

## Examples

See [`examples/`](examples):

- [`swift/UsageC.swift`](examples/swift/UsageC.swift) — C API from Swift
- [`swift/UsageCxx.swift`](examples/swift/UsageCxx.swift) — `tdb::*` from Swift (C++ interop)
- [`objc/UsageC.m`](examples/objc/UsageC.m) — C API from Objective-C
- [`objcpp/UsageCxx.mm`](examples/objcpp/UsageCxx.mm) — `tdb::*` from Objective-C++
