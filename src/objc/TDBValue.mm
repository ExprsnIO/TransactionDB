//
//  TDBValue.mm
//  Objective-C++ implementation. Wraps tdb::sql::Value via shared_ptr so
//  that NSCopying is cheap (TDBValue copies share the same underlying
//  value). The native value is heap-allocated; this is fine because Value
//  itself is small and the cost is amortized by the shared_ptr's reference
//  counting.
//

#import "TDBValue.h"
#import "TDBTypes.h"
#import "tdb/sql/executor.h"

#include <memory>

NSString * const TDBErrorOffendingSQLKey = @"TDBOffendingSQL";
NSString * const TDBErrorOSStatusKey     = @"TDBOSStatus";
NSErrorDomain const TDBErrorDomain       = @"net.rickholland.tdb";

namespace {

using NativeValue = tdb::sql::Value;
using VT = NativeValue::Type;

// ── Type-tag translation (NS <-> wire) ──────────────────────────────────
TDBValueType tdb_objc_type_from_native(VT t) {
    switch (t) {
        case VT::NULL_VAL:      return TDBValueTypeNull;
        case VT::INT64:         return TDBValueTypeInteger;
        case VT::FLOAT64:       return TDBValueTypeFloat;
        case VT::STRING:        return TDBValueTypeString;
        case VT::BOOL:          return TDBValueTypeBoolean;
        case VT::BLOB:          return TDBValueTypeBlob;
        case VT::DATE_VAL:      return TDBValueTypeDate;
        case VT::TIME_VAL:      return TDBValueTypeTime;
        case VT::TIMESTAMP_VAL: return TDBValueTypeTimestamp;
        case VT::TIMESTAMP_TZ:  return TDBValueTypeTimestampTZ;
        case VT::DECIMAL:       return TDBValueTypeDecimal;
        case VT::UUID:          return TDBValueTypeUUID;
        case VT::INTERVAL:      return TDBValueTypeInterval;
        case VT::ENUM_VAL:      return TDBValueTypeEnum;
        case VT::BIT_VAL:       return TDBValueTypeBit;
        case VT::JSON_VAL:      return TDBValueTypeJSON;
        case VT::XML_VAL:       return TDBValueTypeXML;
        case VT::COMPOSITE:     return TDBValueTypeComposite;
        case VT::ARRAY:         return TDBValueTypeArray;
        case VT::MULTISET:      return TDBValueTypeMultiset;
        case VT::GEOMETRY:      return TDBValueTypeGeometry;
        case VT::VARBINARY:     return TDBValueTypeVarbinary;
    }
    return TDBValueTypeNull;
}

// Epoch: TDB's TIMESTAMP_VAL is microseconds since 2000-01-01 UTC.
// NSDate's reference is 2001-01-01 UTC. Convert by adjusting one calendar year.
constexpr int64_t kMicrosPerDay = 86400LL * 1000000LL;
constexpr int64_t kSecondsFromTDB2000ToNS2001 = 31622400; // 366 days (2000 is leap)

NSDate *nsdate_from_micros_since_2000(int64_t micros) {
    double seconds_since_tdb_epoch = (double)micros / 1.0e6;
    return [NSDate dateWithTimeIntervalSinceReferenceDate:
        seconds_since_tdb_epoch - kSecondsFromTDB2000ToNS2001];
}

int64_t micros_since_2000_from_nsdate(NSDate *d) {
    double s_from_ns_ref = d.timeIntervalSinceReferenceDate;
    return (int64_t)((s_from_ns_ref + kSecondsFromTDB2000ToNS2001) * 1.0e6);
}

} // namespace

@implementation TDBValue {
    std::shared_ptr<NativeValue> _native;
}

#pragma mark - NSSecureCoding

+ (BOOL)supportsSecureCoding { return YES; }

- (instancetype)initWithCoder:(NSCoder *)coder {
    self = [super init];
    if (!self) return nil;
    NSData *blob = [coder decodeObjectOfClass:[NSData class] forKey:@"native"];
    // We persist via the engine's wire format — see TDBResultSet for the
    // matching serialize path. For now, we round-trip via canonical string.
    NSString *s = [coder decodeObjectOfClass:[NSString class] forKey:@"str"];
    NSNumber *t = [coder decodeObjectOfClass:[NSNumber class] forKey:@"type"];
    _native = std::make_shared<NativeValue>();
    if (s) _native->str_val = std::string(s.UTF8String);
    if (t) _native->type = (VT)t.integerValue;
    (void)blob;
    return self;
}

- (void)encodeWithCoder:(NSCoder *)coder {
    [coder encodeObject:[NSString stringWithUTF8String:_native->str_val.c_str()] forKey:@"str"];
    [coder encodeObject:@((NSInteger)_native->type) forKey:@"type"];
}

- (id)copyWithZone:(nullable NSZone *)zone {
    TDBValue *copy = [[TDBValue allocWithZone:zone] init];
    copy->_native = _native; // shared_ptr — cheap; values are immutable from
                             // the public API surface so sharing is safe.
    return copy;
}

#pragma mark - Internal helpers

+ (instancetype)_wrapNative:(NativeValue)v {
    TDBValue *out = [[TDBValue alloc] init];
    out->_native = std::make_shared<NativeValue>(std::move(v));
    return out;
}

- (const NativeValue &)nativeRef { return *_native; }

#pragma mark - Construction

+ (instancetype)null                                       { return [self _wrapNative:NativeValue::make_null()]; }
+ (instancetype)valueWithInteger:(int64_t)integer          { return [self _wrapNative:NativeValue::make_int(integer)]; }
+ (instancetype)valueWithDouble:(double)real               { return [self _wrapNative:NativeValue::make_float(real)]; }
+ (instancetype)valueWithBool:(BOOL)boolean                { return [self _wrapNative:NativeValue::make_bool(boolean)]; }

+ (instancetype)valueWithString:(NSString *)string {
    return [self _wrapNative:NativeValue::make_string(std::string(string.UTF8String))];
}

+ (instancetype)valueWithData:(NSData *)data {
    return [self _wrapNative:NativeValue::make_blob(
        std::string((const char *)data.bytes, data.length))];
}

+ (instancetype)valueWithVarbinary:(NSData *)data {
    return [self _wrapNative:NativeValue::make_varbinary(
        std::string((const char *)data.bytes, data.length))];
}

+ (instancetype)valueWithDate:(NSDate *)date {
    return [self _wrapNative:NativeValue{
        .type = VT::TIMESTAMP_VAL,
        .int_val = micros_since_2000_from_nsdate(date),
    }];
}

+ (instancetype)valueWithUUID:(NSUUID *)uuid {
    uuid_t bytes;
    [uuid getUUIDBytes:bytes];
    char buf[37];
    snprintf(buf, sizeof(buf),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        bytes[0],bytes[1],bytes[2],bytes[3], bytes[4],bytes[5], bytes[6],bytes[7],
        bytes[8],bytes[9], bytes[10],bytes[11],bytes[12],bytes[13],bytes[14],bytes[15]);
    return [self _wrapNative:NativeValue::make_uuid(std::string(buf, 36))];
}

+ (instancetype)valueWithDecimal:(NSDecimalNumber *)decimal {
    // Use the canonical text form to avoid double-precision loss.
    NSString *s = [decimal stringValue];
    NSRange dot = [s rangeOfString:@"."];
    NSInteger scale = (dot.location == NSNotFound) ? 0
                     : (NSInteger)(s.length - dot.location - 1);
    return [self _wrapNative:NativeValue::make_decimal(std::string(s.UTF8String), (int)scale)];
}

+ (instancetype)valueWithJSONText:(NSString *)json {
    return [self _wrapNative:NativeValue::make_json(std::string(json.UTF8String))];
}

+ (instancetype)valueWithXMLText:(NSString *)xml {
    return [self _wrapNative:NativeValue::make_xml(std::string(xml.UTF8String))];
}

+ (instancetype)valueWithWKT:(NSString *)wkt SRID:(NSInteger)srid dimensions:(NSInteger)dim {
    return [self _wrapNative:NativeValue::make_geometry(
        std::string(wkt.UTF8String), (int)srid, (int)dim)];
}

+ (instancetype)valueWithArray:(NSArray *)elements {
    std::vector<NativeValue> out;
    out.reserve(elements.count);
    for (id e in elements) {
        TDBValue *v = [TDBValue valueFromObject:e error:nil];
        if (v) out.push_back(v->_native ? *v->_native : NativeValue::make_null());
        else   out.push_back(NativeValue::make_null());
    }
    return [self _wrapNative:NativeValue::make_array(std::move(out))];
}

+ (instancetype)valueWithMultiset:(NSArray *)elements {
    std::vector<NativeValue> out;
    out.reserve(elements.count);
    for (id e in elements) {
        TDBValue *v = [TDBValue valueFromObject:e error:nil];
        if (v) out.push_back(v->_native ? *v->_native : NativeValue::make_null());
        else   out.push_back(NativeValue::make_null());
    }
    return [self _wrapNative:NativeValue::make_multiset(std::move(out))];
}

+ (instancetype)valueWithCompositeName:(NSString *)typeName
                                 fields:(NSDictionary<NSString *, id> *)fields {
    // Composite fields are positional in the wire format, so we sort by key
    // for a deterministic round-trip when no order is given.
    NSArray<NSString *> *keys = [fields.allKeys sortedArrayUsingSelector:@selector(compare:)];
    std::vector<NativeValue> out;
    out.reserve(keys.count);
    for (NSString *k in keys) {
        TDBValue *v = [TDBValue valueFromObject:fields[k] error:nil];
        out.push_back(v ? *v->_native : NativeValue::make_null());
    }
    return [self _wrapNative:NativeValue::make_composite(
        std::string(typeName.UTF8String), std::move(out))];
}

+ (nullable instancetype)valueFromObject:(id)object
                                    error:(NSError * _Nullable __autoreleasing * _Nullable)error {
    if (!object || object == [NSNull null]) return [self null];
    if ([object isKindOfClass:[TDBValue class]]) return object;
    if ([object isKindOfClass:[NSString class]]) return [self valueWithString:object];
    if ([object isKindOfClass:[NSNumber class]]) {
        NSNumber *n = object;
        // Distinguish BOOL from numeric — CFNumber tagging gives us this.
        if (CFGetTypeID((__bridge CFTypeRef)n) == CFBooleanGetTypeID())
            return [self valueWithBool:n.boolValue];
        // Pick INT64 vs FLOAT based on objCType.
        const char *t = n.objCType;
        if (t && (t[0] == 'f' || t[0] == 'd')) return [self valueWithDouble:n.doubleValue];
        return [self valueWithInteger:n.longLongValue];
    }
    if ([object isKindOfClass:[NSData class]])           return [self valueWithData:object];
    if ([object isKindOfClass:[NSUUID class]])           return [self valueWithUUID:object];
    if ([object isKindOfClass:[NSDate class]])           return [self valueWithDate:object];
    if ([object isKindOfClass:[NSDecimalNumber class]])  return [self valueWithDecimal:object];
    if ([object isKindOfClass:[NSArray class]])          return [self valueWithArray:object];
    if ([object isKindOfClass:[NSDictionary class]]) {
        NSDictionary *d = object;
        NSString *typeName = d[@"__type__"] ?: @"";
        NSMutableDictionary *fields = [d mutableCopy];
        [fields removeObjectForKey:@"__type__"];
        return [self valueWithCompositeName:typeName fields:fields];
    }
    if (error) {
        *error = [NSError errorWithDomain:TDBErrorDomain
                                     code:TDBErrorCodeTypeBridge
                                 userInfo:@{
            NSLocalizedDescriptionKey: [NSString stringWithFormat:
                @"No bridge from %@ to TDBValue", NSStringFromClass([object class])]
        }];
    }
    return nil;
}

#pragma mark - Properties

- (TDBValueType)valueType { return tdb_objc_type_from_native(_native->type); }
- (BOOL)isNull            { return _native->is_null(); }
- (nullable NSString *)compositeTypeName {
    if (_native->type != VT::COMPOSITE) return nil;
    return [NSString stringWithUTF8String:_native->str_val.c_str()];
}

#pragma mark - Bridging back

- (id)toObject {
    switch (_native->type) {
        case VT::NULL_VAL: return [NSNull null];
        case VT::INT64:    return @(_native->int_val);
        case VT::FLOAT64:  return @(_native->float_val);
        case VT::STRING:   return [NSString stringWithUTF8String:_native->str_val.c_str()];
        case VT::BOOL:     return _native->bool_val ? @YES : @NO;
        case VT::BLOB:     return [NSData dataWithBytes:_native->str_val.data() length:_native->str_val.size()];
        case VT::VARBINARY:return [NSData dataWithBytes:_native->str_val.data() length:_native->str_val.size()];
        case VT::DATE_VAL: return nsdate_from_micros_since_2000(_native->int_val * kMicrosPerDay);
        case VT::TIMESTAMP_VAL: return nsdate_from_micros_since_2000(_native->int_val);
        case VT::TIMESTAMP_TZ:  return nsdate_from_micros_since_2000(_native->int_val);
        case VT::UUID: {
            NSString *s = [NSString stringWithUTF8String:_native->str_val.c_str()];
            return [[NSUUID alloc] initWithUUIDString:s];
        }
        case VT::DECIMAL: {
            return [NSDecimalNumber decimalNumberWithString:
                [NSString stringWithUTF8String:_native->str_val.c_str()]];
        }
        case VT::JSON_VAL:
        case VT::XML_VAL:
        case VT::BIT_VAL:
        case VT::GEOMETRY:
            return [NSString stringWithUTF8String:_native->str_val.c_str()];
        case VT::ENUM_VAL:
            return @(_native->int_val);
        case VT::INTERVAL: {
            // Months + microseconds → NSDateComponents.
            NSDateComponents *c = [[NSDateComponents alloc] init];
            c.month = (NSInteger)_native->int_val;
            c.second = (NSInteger)(_native->int_val_2 / 1000000);
            return c;
        }
        case VT::ARRAY:
        case VT::MULTISET: {
            NSMutableArray *out = [NSMutableArray array];
            if (_native->composite_fields) {
                for (auto &e : *_native->composite_fields) {
                    [out addObject:[TDBValue _wrapNative:e].toObject];
                }
            }
            return [out copy];
        }
        case VT::COMPOSITE: {
            NSMutableDictionary *out = [NSMutableDictionary dictionary];
            out[@"__type__"] = [NSString stringWithUTF8String:_native->str_val.c_str()];
            if (_native->composite_fields) {
                NSInteger i = 0;
                for (auto &e : *_native->composite_fields) {
                    out[[NSString stringWithFormat:@"f%ld", (long)(i++)]] =
                        [TDBValue _wrapNative:e].toObject;
                }
            }
            return [out copy];
        }
    }
    return [NSNull null];
}

- (NSString *)stringValue {
    return [NSString stringWithUTF8String:_native->to_string().c_str()];
}

- (int64_t)integerValue { return _native->int_val; }
- (double)doubleValue {
    if (_native->type == VT::FLOAT64) return _native->float_val;
    if (_native->type == VT::INT64)   return (double)_native->int_val;
    if (_native->type == VT::DECIMAL) return std::stod(_native->str_val);
    return std::nan("");
}
- (BOOL)boolValue { return _native->bool_val; }

- (nullable NSString *)stringContents {
    switch (_native->type) {
        case VT::STRING: case VT::JSON_VAL: case VT::XML_VAL:
        case VT::BIT_VAL: case VT::GEOMETRY: case VT::DECIMAL: case VT::UUID:
            return [NSString stringWithUTF8String:_native->str_val.c_str()];
        default: return nil;
    }
}

- (nullable NSData *)dataContents {
    if (_native->type != VT::BLOB && _native->type != VT::VARBINARY) return nil;
    return [NSData dataWithBytes:_native->str_val.data() length:_native->str_val.size()];
}

- (nullable NSUUID *)uuidContents {
    if (_native->type != VT::UUID) return nil;
    return [[NSUUID alloc] initWithUUIDString:
        [NSString stringWithUTF8String:_native->str_val.c_str()]];
}

- (nullable NSDate *)dateContents {
    switch (_native->type) {
        case VT::DATE_VAL: return nsdate_from_micros_since_2000(_native->int_val * kMicrosPerDay);
        case VT::TIMESTAMP_VAL:
        case VT::TIMESTAMP_TZ:
            return nsdate_from_micros_since_2000(_native->int_val);
        default: return nil;
    }
}

- (nullable NSDecimalNumber *)decimalContents {
    if (_native->type != VT::DECIMAL) return nil;
    return [NSDecimalNumber decimalNumberWithString:
        [NSString stringWithUTF8String:_native->str_val.c_str()]];
}

- (nullable NSArray<TDBValue *> *)arrayContents {
    if (_native->type != VT::ARRAY && _native->type != VT::MULTISET) return nil;
    if (!_native->composite_fields) return @[];
    NSMutableArray<TDBValue *> *out = [NSMutableArray arrayWithCapacity:_native->composite_fields->size()];
    for (auto &e : *_native->composite_fields) [out addObject:[TDBValue _wrapNative:e]];
    return [out copy];
}

- (nullable NSDictionary<NSString *, TDBValue *> *)compositeFields {
    if (_native->type != VT::COMPOSITE) return nil;
    if (!_native->composite_fields) return @{};
    NSMutableDictionary<NSString *, TDBValue *> *out = [NSMutableDictionary dictionary];
    NSInteger i = 0;
    for (auto &e : *_native->composite_fields) {
        out[[NSString stringWithFormat:@"f%ld", (long)(i++)]] = [TDBValue _wrapNative:e];
    }
    return [out copy];
}

#pragma mark - NSObject

- (NSString *)description {
    return [NSString stringWithFormat:@"<TDBValue %@: %@>",
        @(self.valueType), [self stringValue]];
}

- (BOOL)isEqual:(id)other {
    if (![other isKindOfClass:[TDBValue class]]) return NO;
    TDBValue *o = other;
    return _native->compare(*o->_native) == 0;
}

- (NSUInteger)hash {
    return [[self stringValue] hash] ^ (NSUInteger)_native->type;
}

@end
