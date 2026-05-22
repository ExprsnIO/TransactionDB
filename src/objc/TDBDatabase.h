//
//  TDBDatabase.h
//  Main entrypoint for the framework. Mirrors the conventions of
//  Network.framework's NWConnection: open / close lifecycle, paired sync
//  and async APIs, completion handlers always dispatched on the caller's
//  queue, NS_DESIGNATED_INITIALIZER, NS_UNAVAILABLE on default init.
//

#ifndef TDB_OBJC_DATABASE_H
#define TDB_OBJC_DATABASE_H

#import <Foundation/Foundation.h>
#import "TDBTypes.h"
#import "TDBValue.h"
#import "TDBResultSet.h"

NS_ASSUME_NONNULL_BEGIN

@protocol OS_tdb_database <NSObject>
@end

NS_SWIFT_NAME(TDBDatabase)
@interface TDBDatabase : NSObject <OS_tdb_database>

/// The on-disk URL of this database, if opened from a file. nil for
/// in-memory instances.
@property (nonatomic, copy, readonly, nullable) NSURL *databaseURL;

/// Queue used to serialize internal access. Async APIs hop here, do work,
/// then re-dispatch the completion handler on the caller's queue.
@property (nonatomic, strong, readonly) dispatch_queue_t internalQueue;

#pragma mark - Lifecycle

/// In-memory database — useful for tests and ephemeral caches.
- (instancetype)init NS_DESIGNATED_INITIALIZER;

/// Open or create a file-backed database. Synchronous; returns nil on
/// failure with `error` populated.
- (nullable instancetype)initWithURL:(NSURL *)url
                                error:(NSError * _Nullable __autoreleasing * _Nullable)error
    NS_DESIGNATED_INITIALIZER;

/// Async open. The completion handler is always dispatched on `queue`.
+ (void)openDatabaseAtURL:(NSURL *)url
                     queue:(dispatch_queue_t)queue
         completionHandler:(void (^)(TDBDatabase * _Nullable db,
                                     NSError * _Nullable error))handler
    NS_SWIFT_NAME(open(at:queue:completionHandler:));

/// Persist any in-memory state to disk. No-op for in-memory databases.
- (BOOL)save:(NSError * _Nullable __autoreleasing * _Nullable)error;

/// Close the database. Any subsequent `execute:` returns an error.
- (void)close;

#pragma mark - Execution

/// Synchronous SQL execution. Returns a TDBResultSet on success, nil on a
/// parse / type / engine error with `error` populated. For statements
/// that don't produce rows (INSERT/UPDATE/DELETE/DDL) the result set has
/// `rowsAffected` set and `rowCount == 0`.
- (nullable TDBResultSet *)executeSQL:(NSString *)sql
                                 error:(NSError * _Nullable __autoreleasing * _Nullable)error
    NS_SWIFT_NAME(execute(_:));

/// Parameterized version. Question-mark placeholders are substituted from
/// the `parameters` array. Each element is converted via
/// `+[TDBValue valueFromObject:]`.
- (nullable TDBResultSet *)executeSQL:(NSString *)sql
                            parameters:(nullable NSArray *)parameters
                                 error:(NSError * _Nullable __autoreleasing * _Nullable)error
    NS_SWIFT_NAME(execute(_:parameters:));

/// Async execution. The completion handler is dispatched on `queue`.
- (void)executeSQL:(NSString *)sql
        parameters:(nullable NSArray *)parameters
              queue:(dispatch_queue_t)queue
  completionHandler:(TDBExecuteCompletionHandler)handler
    NS_SWIFT_NAME(execute(_:parameters:queue:completionHandler:));

#pragma mark - Transactions

- (BOOL)beginTransaction:(NSError * _Nullable __autoreleasing * _Nullable)error;
- (BOOL)commit:(NSError * _Nullable __autoreleasing * _Nullable)error;
- (BOOL)rollback:(NSError * _Nullable __autoreleasing * _Nullable)error;

/// Execute `block` inside BEGIN/COMMIT. If the block throws or returns NO
/// via the inout BOOL, the transaction is rolled back and the failure
/// propagated.
- (BOOL)transaction:(NS_NOESCAPE BOOL (^)(TDBDatabase *db, NSError ** error))block
              error:(NSError * _Nullable __autoreleasing * _Nullable)error;

@end

NS_ASSUME_NONNULL_END

#endif /* TDB_OBJC_DATABASE_H */
