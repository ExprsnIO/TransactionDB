# src/objc — Objective-C/Objective-C++ framework wrapping libtdb.
#
# This subdirectory is only added on Apple platforms. The output is a
# framework bundle named TDB.framework with the umbrella header TDB.h and
# the module map for clean Swift import.
#
# Add to a project with:
#   add_subdirectory(src/objc)
# from a parent CMakeLists.txt, conditional on APPLE.

if(NOT APPLE)
    message(STATUS "TDB ObjC framework: skipping (non-Apple platform)")
    return()
endif()

cmake_minimum_required(VERSION 3.20)

# Compile every .mm under ObjC++ rules.
set(TDB_OBJC_SOURCES
    TDBValue.mm
    TDBResultSet.mm
    TDBDatabase.mm
    TDBCrypto.mm
    TDBKeychain.mm
)
set(TDB_OBJC_HEADERS
    TDB.h
    TDBTypes.h
    TDBValue.h
    TDBResultSet.h
    TDBDatabase.h
    TDBCrypto.h
    TDBKeychain.h
)

add_library(TDBFramework SHARED ${TDB_OBJC_SOURCES} ${TDB_OBJC_HEADERS})

# C++20 / Objective-C++ — ARC is required so callers don't have to think
# about retain/release in the public API surface.
set_target_properties(TDBFramework PROPERTIES
    OUTPUT_NAME "TDB"
    FRAMEWORK TRUE
    FRAMEWORK_VERSION A
    MACOSX_FRAMEWORK_IDENTIFIER "net.rickholland.tdb"
    PUBLIC_HEADER "${TDB_OBJC_HEADERS}"
    XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES
    XCODE_ATTRIBUTE_CLANG_ENABLE_MODULES YES
    XCODE_ATTRIBUTE_DEFINES_MODULE YES
    XCODE_ATTRIBUTE_CLANG_CXX_LANGUAGE_STANDARD "c++20"
    XCODE_ATTRIBUTE_CLANG_CXX_LIBRARY "libc++"
)
# For non-Xcode generators (make, ninja), pass the ARC and module flags
# explicitly.
target_compile_options(TDBFramework PRIVATE
    -fobjc-arc
    -fmodules
    -fcxx-modules
    $<$<COMPILE_LANGUAGE:OBJCXX>:-std=c++20>
)

target_include_directories(TDBFramework
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${PROJECT_SOURCE_DIR}/include
)

# Link the underlying libtdb stack + system frameworks. tdb has a unique
# transitive closure here so we list everything explicitly.
target_link_libraries(TDBFramework PRIVATE
    tdb tdb_sql tdb_catalog tdb_doc tdb_script tdb_persistence tdb_core tdb_index
    "-framework Foundation"
    "-framework Security"
)

# Drop the module map into the framework's Modules directory so the Swift
# importer finds it.
add_custom_command(TARGET TDBFramework POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:TDBFramework>/Modules
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_CURRENT_SOURCE_DIR}/module.modulemap
        $<TARGET_FILE_DIR:TDBFramework>/Modules/module.modulemap
)
