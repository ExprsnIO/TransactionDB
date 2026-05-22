//
//  TDBTypes.h
//  Error domain + codes, value-type tag enum, and CF-style opaque vended
//  types. Modeled after Network.framework's <Network/Network.h>: every
//  high-level Objective-C class also has a low-level C-named typedef
//  (`tdb_database_t`, `tdb_value_t`) so callers writing in C-flavored
//  Objective-C can pass these around without typing class names.
//

#ifndef TDB_OBJC_TYPES_H
#define TDB_OBJC_TYPES_H

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

#pragma mark - Error domain

FOUNDATION_EXPORT NSErrorDomain const TDBErrorDomain NS_SWIFT_NAME(TDBError.errorDomain);

/// Top-level error codes vended by the TDB framework. Maps 1:1 to
/// `tdb_status_t` from the C API, with -1 reserved for OBJC-side failures
/// like type-bridge mismatches.
typedef NS_ERROR_ENUM(TDBErrorDomain, TDBErrorCode) {
    TDBErrorCodeOK              = 0,
    TDBErrorCodeNotFound        = 1,
    TDBErrorCodeIO              = 2,
    TDBErrorCodeInvalidArgument = 3,
    TDBErrorCodeOutOfMemory     = 4,
    TDBErrorCodeBufferTooSmall  = 5,
    TDBErrorCodeIntegrity       = 6,
    TDBErrorCodeUnsupported     = 7,
    TDBErrorCodeAuth            = 8,
    /// SQL parsing or execution failure — the SQL string is in
    /// userInfo[TDBErrorOffendingSQLKey] and the engine message in
    /// userInfo[NSLocalizedDescriptionKey].
    TDBErrorCodeSQL             = 100,
    /// Type-bridge failure: an NSObject was passed for a value-conversion
    /// site that doesn't have a bridge to a Value::Type.
    TDBErrorCodeTypeBridge      = 101,
    /// Keychain-side failure; an OSStatus is in
    /// userInfo[TDBErrorOSStatusKey].
    TDBErrorCodeKeychain        = 102,
} NS_SWIFT_NAME(TDBError);

FOUNDATION_EXPORT NSString * const TDBErrorOffendingSQLKey;
FOUNDATION_EXPORT NSString * const TDBErrorOSStatusKey;

#pragma mark - Value type tag

/// Wire-level type tag for a `TDBValue`. Matches `tdb::sql::Value::Type`.
typedef NS_ENUM(NSInteger, TDBValueType) {
    TDBValueTypeNull         =  0,
    TDBValueTypeInteger      =  1,  ///< NSNumber/long long
    TDBValueTypeFloat        =  2,  ///< NSNumber/double
    TDBValueTypeString       =  3,  ///< NSString
    TDBValueTypeBoolean      =  4,  ///< NSNumber/BOOL
    TDBValueTypeBlob         =  5,  ///< NSData
    TDBValueTypeDate         =  6,  ///< NSDate (date only)
    TDBValueTypeTime         =  7,  ///< NSDateComponents (hour/min/sec)
    TDBValueTypeTimestamp    =  8,  ///< NSDate
    TDBValueTypeTimestampTZ  =  9,  ///< NSDate + offset
    TDBValueTypeDecimal      = 10,  ///< NSDecimalNumber
    TDBValueTypeUUID         = 11,  ///< NSUUID
    TDBValueTypeInterval     = 12,
    TDBValueTypeEnum         = 13,
    TDBValueTypeBit          = 14,
    TDBValueTypeJSON         = 15,  ///< NSString (canonical JSON text)
    TDBValueTypeXML          = 16,  ///< NSString
    TDBValueTypeComposite    = 17,  ///< NSDictionary<NSString *, TDBValue *>
    TDBValueTypeArray        = 18,  ///< NSArray<TDBValue *>
    TDBValueTypeMultiset     = 19,  ///< NSArray<TDBValue *>
    TDBValueTypeGeometry     = 20,  ///< NSString WKT
    TDBValueTypeVarbinary    = 21,  ///< NSData
} NS_SWIFT_NAME(TDBValueType);

#pragma mark - CF-style opaque vended objects (Network.framework idiom)

/// Forward-declared protocols give each vended object a unique pointer
/// type while remaining a regular `NSObject *` under ARC.
@protocol OS_tdb_database;
@protocol OS_tdb_value;
@protocol OS_tdb_result_set;
@protocol OS_tdb_crypto_context;

typedef NSObject<OS_tdb_database>       *tdb_database_t       NS_SWIFT_NAME(TDBDatabaseRef);
typedef NSObject<OS_tdb_value>          *tdb_value_t          NS_SWIFT_NAME(TDBValueRef);
typedef NSObject<OS_tdb_result_set>     *tdb_result_set_t     NS_SWIFT_NAME(TDBResultSetRef);
typedef NSObject<OS_tdb_crypto_context> *tdb_crypto_context_t NS_SWIFT_NAME(TDBCryptoContextRef);

/// Lightweight completion-handler typedefs, matching Apple framework style.
typedef void (^TDBExecuteCompletionHandler)(NSObject * _Nullable resultSet,
                                             NSError * _Nullable error)
    NS_SWIFT_NAME(TDBExecuteCompletionHandler);

typedef void (^TDBOpenCompletionHandler)(NSError * _Nullable error)
    NS_SWIFT_NAME(TDBOpenCompletionHandler);

NS_ASSUME_NONNULL_END

#endif /* TDB_OBJC_TYPES_H */
