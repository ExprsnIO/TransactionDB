/*
 * TransactionDB.framework — umbrella header
 *
 * This is the public umbrella header for the framework. It exposes the C API
 * (transactiondb.h) to every client language (C, Objective-C, Swift). The
 * C++17 RAII wrapper (transactiondb.hpp) is intentionally NOT pulled in here:
 * it lives in the framework's `CXX` submodule (see module.modulemap) so that
 * pure C / Objective-C / Swift-without-C++-interop clients are never forced to
 * parse C++ headers.
 *
 *   - C and Objective-C:  #import <TransactionDB/TransactionDB.h>
 *   - Objective-C++:      #import <TransactionDB/transactiondb.hpp>   (tdb::*)
 *   - Swift:              import TransactionDB                        (C API)
 *   - Swift (C++ interop) import TransactionDB                        (tdb::*)
 *                         requires -cxx-interoperability-mode=default
 */
#import <TransactionDB/transactiondb.h>
