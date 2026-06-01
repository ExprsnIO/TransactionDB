#!/usr/bin/env bash
#
# Build TransactionDB.framework for macOS, iOS (device) and iOS (simulator),
# then bundle them into a single multi-platform TransactionDB.xcframework.
#
# Requires: macOS with Xcode + CMake (>= 3.16). Run from anywhere:
#
#     apple/build-xcframework.sh
#
# Output:
#     dist/TransactionDB.xcframework
#
# Environment overrides:
#     CONFIG=Release            CMake/Xcode build configuration
#     IOS_DEPLOYMENT_TARGET=13.0
#     MACOS_DEPLOYMENT_TARGET=11.0
#     TDB_BUILD_LUA=OFF         Lua fetches over the network and is awkward to
#                               cross-compile for iOS, so it is off by default.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_ROOT="${ROOT}/build-apple"
DIST="${ROOT}/dist"
CONFIG="${CONFIG:-Release}"
IOS_DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET:-13.0}"
MACOS_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET:-11.0}"
TDB_BUILD_LUA="${TDB_BUILD_LUA:-OFF}"

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "error: building an Apple framework requires macOS with Xcode." >&2
  exit 1
fi

COMMON_ARGS=(
  -G Xcode
  -DTDB_BUILD_FRAMEWORK=ON
  -DTDB_BUILD_TESTS=OFF
  -DTDB_BUILD_CLI=OFF
  -DTDB_BUILD_LUA="${TDB_BUILD_LUA}"
  -DCMAKE_XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH=NO
)

# build_slice <name> <extra cmake args...>
build_slice() {
  local name="$1"; shift
  local dir="${BUILD_ROOT}/${name}"
  echo "==> configuring ${name}"
  cmake -S "${ROOT}" -B "${dir}" "${COMMON_ARGS[@]}" "$@"
  echo "==> building ${name} (${CONFIG})"
  cmake --build "${dir}" --config "${CONFIG}"
}

# Path to the produced framework for a slice.
slice_framework() {
  local name="$1"
  echo "${BUILD_ROOT}/${name}/${CONFIG}/TransactionDB.framework"
}

rm -rf "${DIST}/TransactionDB.xcframework"
mkdir -p "${DIST}"

build_slice macos \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET}"

build_slice ios-device \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES="arm64" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET}"

build_slice ios-simulator \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphonesimulator \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET}"

# The Xcode generator nests per-platform output under <CONFIG>-<sdk>; fall back
# to <CONFIG> for the macOS slice.
find_framework() {
  local name="$1" sdk="$2"
  local d
  for d in \
    "${BUILD_ROOT}/${name}/${CONFIG}-${sdk}/TransactionDB.framework" \
    "${BUILD_ROOT}/${name}/${CONFIG}/TransactionDB.framework"; do
    if [[ -d "$d" ]]; then echo "$d"; return 0; fi
  done
  echo "error: could not locate TransactionDB.framework for slice '${name}'" >&2
  return 1
}

MACOS_FW="$(find_framework macos macosx)"
IOS_FW="$(find_framework ios-device iphoneos)"
SIM_FW="$(find_framework ios-simulator iphonesimulator)"

echo "==> creating TransactionDB.xcframework"
xcodebuild -create-xcframework \
  -framework "${MACOS_FW}" \
  -framework "${IOS_FW}" \
  -framework "${SIM_FW}" \
  -output "${DIST}/TransactionDB.xcframework"

echo "done: ${DIST}/TransactionDB.xcframework"
