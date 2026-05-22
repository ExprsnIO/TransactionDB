//
//  TDBValue.h
//  An Objective-C wrapper around tdb::sql::Value with bridges to native
//  Foundation types. Modeled after NSNumber's "any-of-many-primitives"
//  pattern with explicit type tags. TDBValue conforms to OS_tdb_value so it
//  can be passed where a `tdb_value_t` is expected, and to NSCopying /
//  NSSecureCoding for use with Foundation collections and archives.
//

#ifndef TDB_OBJC_VALUE_H
#define TDB_OBJC_VALUE_H

#import <Foundation/Foundation.h>
#import "TDBTypes.h"

NS_ASSUME_NONNULL_BEGIN

@protocol OS_tdb_value <NSObject>
@end

NS_SWIFT_NAME(TDBValue)
@interface TDBValue : NSObject <OS_tdb_value, NSCopying, NSSecureCoding>

/// The wire-level type tag of this value.
@property (nonatomic, readonly) TDBValueType valueType;

/// Convenience predicate — true when `valueType == TDBValueTypeNull`.
@property (nonatomic, readonly, getter=isNull) BOOL null;

#pragma mark - Construction

+ (instancetype)null;
+ (instancetype)valueWithInteger:(int64_t)integer NS_SWIFT_NAME(init(integer:));
+ (instancetype)valueWithDouble:(double)real     NS_SWIFT_NAME(init(double:));
+ (instancetype)valueWithBool:(BOOL)boolean      NS_SWIFT_NAME(init(bool:));
+ (instancetype)valueWithString:(NSString *)string;
+ (instancetype)valueWithData:(NSData *)data;
+ (instancetype)valueWithVarbinary:(NSData *)data;
+ (instancetype)valueWithDate:(NSDate *)date;
+ (instancetype)valueWithUUID:(NSUUID *)uuid;
+ (instancetype)valueWithDecimal:(NSDecimalNumber *)decimal;
+ (instancetype)valueWithJSONText:(NSString *)json;
+ (instancetype)valueWithXMLText:(NSString *)xml;
+ (instancetype)valueWithWKT:(NSString *)wkt
                        SRID:(NSInteger)srid
                  dimensions:(NSInteger)dim;

/// ARRAY constructor. Elements may be `TDBValue` instances or any
/// Foundation type with a registered bridge (see `+valueFromObject:`).
+ (instancetype)valueWithArray:(NSArray *)elements;

/// MULTISET constructor (semantic distinction from ARRAY: order-insensitive
/// equality; multiplicity matters).
+ (instancetype)valueWithMultiset:(NSArray *)elements;

/// COMPOSITE constructor. The dictionary maps field names to TDBValue or
/// any bridgeable type. The composite-type name is stored separately.
+ (instancetype)valueWithCompositeName:(NSString *)typeName
                                 fields:(NSDictionary<NSString *, id> *)fields;

/// Generic bridge: inspects the runtime class of `object` and routes to the
/// matching factory. Returns `nil` and fills `error` if no bridge exists.
+ (nullable instancetype)valueFromObject:(id)object
                                    error:(NSError * _Nullable __autoreleasing * _Nullable)error;

#pragma mark - Bridging back to Foundation

/// Returns the closest native Foundation type for this value's tag:
/// STRING -> NSString, INT64 -> NSNumber, ARRAY -> NSArray, COMPOSITE ->
/// NSDictionary, etc. NULL returns NSNull.
- (id)toObject;

/// Canonical string form (matches what the SQL engine would print).
- (NSString *)stringValue;

/// Available when valueType is TDBValueTypeInteger / Boolean / Date /
/// Time / Timestamp / Enum. Returns 0 otherwise.
- (int64_t)integerValue;

/// Available when valueType is TDBValueTypeFloat / Decimal. Coerces Integer
/// to double; returns NaN for unsupported tags.
- (double)doubleValue;

- (BOOL)boolValue;

/// Returns nil unless valueType is String / JSON / XML / Geometry.
- (nullable NSString *)stringContents;

/// Returns nil unless valueType is Blob / Varbinary.
- (nullable NSData *)dataContents;

/// Returns nil unless valueType is UUID.
- (nullable NSUUID *)uuidContents;

/// Returns nil unless valueType is Date / Timestamp / TimestampTZ.
- (nullable NSDate *)dateContents;

/// Returns nil unless valueType is Decimal.
- (nullable NSDecimalNumber *)decimalContents;

/// ARRAY / MULTISET elements as NSArray<TDBValue *> *. nil for other tags.
- (nullable NSArray<TDBValue *> *)arrayContents;

/// COMPOSITE fields as NSDictionary<NSString *, TDBValue *> *. nil for
/// non-composite tags. The composite type name is in `compositeTypeName`.
- (nullable NSDictionary<NSString *, TDBValue *> *)compositeFields;
@property (nonatomic, copy, readonly, nullable) NSString *compositeTypeName;

@end

NS_ASSUME_NONNULL_END

#endif /* TDB_OBJC_VALUE_H */
