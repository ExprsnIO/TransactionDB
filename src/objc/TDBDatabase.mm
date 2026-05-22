//
//  TDBDatabase.mm
//  Objective-C++ wrapping of `tdb::Database`. Serialized through a private
//  dispatch queue (the existing C++ class is not thread-safe in the
//  default build; ThreadSafeDatabase does its own locking and could be
//  swapped in here for true concurrent access).
//

#import "TDBDatabase.h"
#import "TDBResultSet.h"
#import "tdb/database.h"

@interface TDBResultSet (Internal)
- (instancetype)initFromNative:(const tdb::sql::ResultSet &)rs;
@end

@interface TDBValue (Internal)
- (const tdb::sql::Value &)nativeRef;
@end

#pragma mark - Parameter binding

// Naive ?-placeholder substitution. Production code should use prepared
// statements; the engine doesn't expose those at the C++ level yet so we
// inline-quote each parameter using its canonical SQL form.
static NSString *expand_parameters(NSString *sql, NSArray * _Nullable params) {
    if (params.count == 0) return sql;
    NSMutableString *out = [NSMutableString stringWithCapacity:sql.length];
    NSUInteger pi = 0;
    NSUInteger n = sql.length;
    for (NSUInteger i = 0; i < n; i++) {
        unichar c = [sql characterAtIndex:i];
        if (c == '?' && pi < params.count) {
            id raw = params[pi++];
            TDBValue *v = [TDBValue valueFromObject:raw error:nil];
            tdb::sql::Value native = v ? [v nativeRef] : tdb::sql::Value::make_null();
            // Quote per SQL rules: NULL, ints/floats unquoted, everything
            // else single-quoted with embedded quotes doubled.
            std::string s = native.to_string();
            using VT = tdb::sql::Value::Type;
            switch (native.type) {
                case VT::NULL_VAL:
                    [out appendString:@"NULL"]; break;
                case VT::INT64: case VT::FLOAT64: case VT::BOOL: case VT::DECIMAL:
                    [out appendString:[NSString stringWithUTF8String:s.c_str()]]; break;
                default: {
                    [out appendString:@"'"];
                    for (char ch : s) {
                        if (ch == '\'') [out appendString:@"''"];
                        else [out appendFormat:@"%c", ch];
                    }
                    [out appendString:@"'"];
                    break;
                }
            }
        } else {
            [out appendFormat:@"%C", c];
        }
    }
    return [out copy];
}

#pragma mark - Class

@implementation TDBDatabase {
    std::unique_ptr<tdb::Database> _db;
    BOOL _closed;
}

#pragma mark - Init

- (instancetype)init {
    self = [super init];
    if (!self) return nil;
    _internalQueue = dispatch_queue_create("net.rickholland.tdb.internal",
                                            DISPATCH_QUEUE_SERIAL);
    _db = std::make_unique<tdb::Database>();
    // In-memory: open with empty path.
    _db->open("");
    return self;
}

- (nullable instancetype)initWithURL:(NSURL *)url
                                error:(NSError * _Nullable __autoreleasing *)error {
    self = [super init];
    if (!self) return nil;
    if (!url.isFileURL) {
        if (error) *error = [NSError errorWithDomain:TDBErrorDomain
                                                 code:TDBErrorCodeInvalidArgument
                                             userInfo:@{
            NSLocalizedDescriptionKey: @"databaseURL must be a file URL"
        }];
        return nil;
    }
    _internalQueue = dispatch_queue_create("net.rickholland.tdb.internal",
                                            DISPATCH_QUEUE_SERIAL);
    _db = std::make_unique<tdb::Database>();
    _databaseURL = [url copy];
    if (!_db->open(std::string(url.path.UTF8String))) {
        if (error) *error = [NSError errorWithDomain:TDBErrorDomain
                                                 code:TDBErrorCodeIO
                                             userInfo:@{
            NSLocalizedDescriptionKey: [NSString stringWithFormat:
                @"Could not open database at %@", url.path]
        }];
        return nil;
    }
    return self;
}

+ (void)openDatabaseAtURL:(NSURL *)url
                     queue:(dispatch_queue_t)queue
         completionHandler:(void (^)(TDBDatabase * _Nullable, NSError * _Nullable))handler {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *err = nil;
        TDBDatabase *db = [[TDBDatabase alloc] initWithURL:url error:&err];
        dispatch_async(queue, ^{ handler(db, err); });
    });
}

#pragma mark - Lifecycle

- (BOOL)save:(NSError * _Nullable __autoreleasing *)error {
    __block BOOL ok = NO;
    dispatch_sync(_internalQueue, ^{
        if (_closed) return;
        ok = _db->save();
    });
    if (!ok && error) {
        *error = [NSError errorWithDomain:TDBErrorDomain code:TDBErrorCodeIO
                                 userInfo:@{NSLocalizedDescriptionKey: @"save failed"}];
    }
    return ok;
}

- (void)close {
    dispatch_sync(_internalQueue, ^{
        if (_closed) return;
        _db->close();
        _closed = YES;
    });
}

#pragma mark - Execution

- (nullable TDBResultSet *)executeSQL:(NSString *)sql
                                 error:(NSError * _Nullable __autoreleasing *)error {
    return [self executeSQL:sql parameters:nil error:error];
}

- (nullable TDBResultSet *)executeSQL:(NSString *)sql
                            parameters:(nullable NSArray *)parameters
                                 error:(NSError * _Nullable __autoreleasing *)error {
    NSString *expanded = expand_parameters(sql, parameters);
    __block TDBResultSet *rs = nil;
    __block NSError *err = nil;
    dispatch_sync(_internalQueue, ^{
        if (_closed) {
            err = [NSError errorWithDomain:TDBErrorDomain code:TDBErrorCodeInvalidArgument
                                  userInfo:@{NSLocalizedDescriptionKey: @"database is closed"}];
            return;
        }
        auto native_rs = _db->execute(std::string(expanded.UTF8String));
        if (!native_rs.success) {
            err = [NSError errorWithDomain:TDBErrorDomain code:TDBErrorCodeSQL
                                  userInfo:@{
                NSLocalizedDescriptionKey: [NSString stringWithUTF8String:
                    native_rs.error_message.empty() ? "SQL error" : native_rs.error_message.c_str()],
                TDBErrorOffendingSQLKey: expanded ?: sql,
            }];
            return;
        }
        rs = [[TDBResultSet alloc] initFromNative:native_rs];
    });
    if (error && err) *error = err;
    return rs;
}

- (void)executeSQL:(NSString *)sql
        parameters:(nullable NSArray *)parameters
              queue:(dispatch_queue_t)queue
  completionHandler:(TDBExecuteCompletionHandler)handler {
    NSString *snapshot = [sql copy];
    NSArray *paramsCopy = [parameters copy];
    dispatch_async(_internalQueue, ^{
        NSError *err = nil;
        TDBResultSet *rs = [self executeSQL:snapshot parameters:paramsCopy error:&err];
        dispatch_async(queue, ^{ handler(rs, err); });
    });
}

#pragma mark - Transactions

- (BOOL)beginTransaction:(NSError * _Nullable __autoreleasing *)error {
    return [self executeSQL:@"BEGIN" error:error] != nil;
}
- (BOOL)commit:(NSError * _Nullable __autoreleasing *)error {
    return [self executeSQL:@"COMMIT" error:error] != nil;
}
- (BOOL)rollback:(NSError * _Nullable __autoreleasing *)error {
    return [self executeSQL:@"ROLLBACK" error:error] != nil;
}

- (BOOL)transaction:(NS_NOESCAPE BOOL (^)(TDBDatabase *, NSError **))block
              error:(NSError * _Nullable __autoreleasing *)error {
    NSError *err = nil;
    if (![self beginTransaction:&err]) {
        if (error) *error = err;
        return NO;
    }
    NSError *block_err = nil;
    BOOL ok = block(self, &block_err);
    if (!ok) {
        NSError *_unused = nil;
        [self rollback:&_unused];
        if (error) *error = block_err;
        return NO;
    }
    return [self commit:error];
}

@end
