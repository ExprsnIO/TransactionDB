# FindLLVMOpt.cmake
#
# Optional LLVM detection for the TransactionDB JIT layer.
#
# When TDB_BUILD_LLVM is ON we run `find_package(LLVM CONFIG)` and, on
# success, the caller receives the cache variables TDB_LLVM_FOUND,
# TDB_LLVM_INCLUDE_DIRS, TDB_LLVM_LIBS and TDB_LLVM_VERSION. On failure
# the caller can either error out (REQUIRED) or fall back to the stubbed
# JIT (the C code in src/jit/ compiles without LLVM and returns
# TDB_UNSUPPORTED at runtime).
#
# Usage:
#   include(FindLLVMOpt)
#   tdb_find_llvm(REQUIRED)         # errors if not found
#   tdb_find_llvm()                 # sets TDB_LLVM_FOUND=FALSE if not found

function(tdb_find_llvm)
  set(_required FALSE)
  foreach(arg ${ARGN})
    if(arg STREQUAL "REQUIRED")
      set(_required TRUE)
    endif()
  endforeach()

  # Prefer the `llvm-config` binary over LLVMConfig.cmake — the latter ships
  # broken imported-target references on some distros (notably Ubuntu 24.04,
  # where LLVMSupport pulls in `zstd::libzstd_shared` even when zstd isn't
  # installed). llvm-config gives us flat -I/-L/-l flags that always work.
  set(LLVM_FOUND FALSE)
  find_program(LLVM_CONFIG_EXE NAMES llvm-config-18 llvm-config-17 llvm-config-16 llvm-config)
  if(LLVM_CONFIG_EXE)
    execute_process(COMMAND ${LLVM_CONFIG_EXE} --version
      OUTPUT_VARIABLE _v OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${LLVM_CONFIG_EXE} --includedir
      OUTPUT_VARIABLE _inc OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${LLVM_CONFIG_EXE} --libdir
      OUTPUT_VARIABLE _libdir OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(EXISTS "${_inc}/llvm-c/Core.h")
      set(LLVM_FOUND TRUE)
      set(LLVM_VERSION ${_v})
      set(LLVM_INCLUDE_DIRS ${_inc})
      set(LLVM_LIBRARY_DIRS ${_libdir})
      # Parse the actual versioned library name out of `--libnames` (yields
      # e.g. `libLLVM-18.so`). Falling back to plain "LLVM" almost always
      # fails on Debian-family installs because the symlink ships only in
      # the version-specific libdir, not in the system-wide search path.
      execute_process(COMMAND ${LLVM_CONFIG_EXE} --libnames
        OUTPUT_VARIABLE _libnames OUTPUT_STRIP_TRAILING_WHITESPACE)
      string(REGEX REPLACE "^lib(.*)\\.(so|a|dylib).*" "\\1" _libname "${_libnames}")
      if(NOT _libname OR _libname STREQUAL "")
        set(_libname "LLVM")
      endif()
      set(LLVM_LIBS "${_libname}")
      message(STATUS "LLVM ${_v} found via ${LLVM_CONFIG_EXE} (shared, -l${_libname})")
    endif()
  endif()

  if(NOT LLVM_FOUND)
    # Last-ditch: try the CMake config package. May fail with broken target
    # references on some distros — caller can fix by installing the listed
    # missing dependency (e.g. libzstd-dev on Ubuntu).
    find_package(LLVM CONFIG QUIET)
    if(LLVM_FOUND)
      message(STATUS "LLVM ${LLVM_PACKAGE_VERSION} found via LLVMConfig.cmake")
      llvm_map_components_to_libnames(LLVM_LIBS
        core support irreader orcjit executionengine native target)
      set(LLVM_VERSION ${LLVM_PACKAGE_VERSION})
    endif()
  endif()

  if(LLVM_FOUND)
    set(TDB_LLVM_FOUND        TRUE                  CACHE INTERNAL "")
    set(TDB_LLVM_INCLUDE_DIRS "${LLVM_INCLUDE_DIRS}" CACHE INTERNAL "")
    set(TDB_LLVM_LIBRARY_DIRS "${LLVM_LIBRARY_DIRS}" CACHE INTERNAL "")
    set(TDB_LLVM_LIBS         "${LLVM_LIBS}"         CACHE INTERNAL "")
    set(TDB_LLVM_VERSION      "${LLVM_VERSION}"      CACHE INTERNAL "")
  else()
    set(TDB_LLVM_FOUND FALSE CACHE INTERNAL "")
    if(_required)
      message(FATAL_ERROR
        "TDB_BUILD_LLVM=ON but no LLVM development headers were found. "
        "Install llvm-18-dev (or set CMAKE_PREFIX_PATH to an LLVM install).")
    else()
      message(STATUS "LLVM dev headers not found — JIT layer will be a stub")
    endif()
  endif()
endfunction()
