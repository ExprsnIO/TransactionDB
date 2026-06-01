# AppleFramework.cmake
#
# Configures the `transactiondb` target as a TransactionDB.framework bundle on
# Apple platforms. Included from the top-level CMakeLists.txt after the target
# is created. A no-op unless building on Apple with -DTDB_BUILD_FRAMEWORK=ON.
#
# The framework vends:
#   - the C API   (include/transactiondb.h)   to C / Objective-C / Swift
#   - the C++ API (include/transactiondb.hpp)  to C++ / Objective-C++ / Swift
#                                              (the latter via C++ interop)
# See apple/module.modulemap for how each language sees the headers.

if(NOT (APPLE AND TDB_BUILD_FRAMEWORK))
  return()
endif()

set(_tdb_apple_dir ${CMAKE_CURRENT_SOURCE_DIR}/apple)

set_target_properties(transactiondb PROPERTIES
  FRAMEWORK TRUE
  FRAMEWORK_VERSION A
  OUTPUT_NAME TransactionDB
  MACOSX_FRAMEWORK_IDENTIFIER net.rickholland.transactiondb
  MACOSX_FRAMEWORK_BUNDLE_VERSION ${PROJECT_VERSION}
  MACOSX_FRAMEWORK_SHORT_VERSION_STRING ${PROJECT_VERSION}
  MACOSX_FRAMEWORK_INFO_PLIST ${_tdb_apple_dir}/Info.plist.in
  # Public headers land in TransactionDB.framework/Headers/.
  PUBLIC_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/transactiondb.h;${CMAKE_CURRENT_SOURCE_DIR}/include/transactiondb.hpp;${_tdb_apple_dir}/TransactionDB.h"
  # Resolve dependent dylib paths relative to the loader (@rpath) so the
  # framework is relocatable inside an app/xcframework.
  MACOSX_RPATH ON
  INSTALL_NAME_DIR "@rpath"
  # Xcode generator: emit a module so `import TransactionDB` works.
  XCODE_ATTRIBUTE_DEFINES_MODULE YES
  XCODE_ATTRIBUTE_CLANG_ENABLE_MODULES YES
)

# Ship our hand-written module map at TransactionDB.framework/Modules/.
# Attaching it as a target source with MACOSX_PACKAGE_LOCATION copies it into
# the bundle without treating it as a compilable input.
target_sources(transactiondb PRIVATE ${_tdb_apple_dir}/module.modulemap)
set_source_files_properties(${_tdb_apple_dir}/module.modulemap PROPERTIES
  MACOSX_PACKAGE_LOCATION Modules
  HEADER_FILE_ONLY ON
)

unset(_tdb_apple_dir)
