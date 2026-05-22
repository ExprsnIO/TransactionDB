//
//  TDBResultSet.mm
//

#import "TDBResultSet.h"
#import "tdb/sql/executor.h"

@interface TDBValue (Internal)
+ (instancetype)_wrapNative:(tdb::sql::Value)v;
@end

#pragma mark - TDBRow

@implementation TDBRow {
    NSArray<TDBValue *> *_values;
    NSArray<NSString *> *_columns;
    NSDictionary<NSString *, NSNumber *> *_columnIndex;
}

- (instancetype)initWithValues:(NSArray<TDBValue *> *)values
                       columns:(NSArray<NSString *> *)columns {
    self = [super init];
    if (!self) return nil;
    _values = [values copy];
    _columns = [columns copy];
    NSMutableDictionary *idx = [NSMutableDictionary dictionaryWithCapacity:columns.count];
    [columns enumerateObjectsUsingBlock:^(NSString *c, NSUInteger i, BOOL *stop) {
        (void)stop;
        idx[c] = @(i);
    }];
    _columnIndex = [idx copy];
    return self;
}

- (NSArray<TDBValue *> *)values     { return _values; }
- (NSArray<NSString *> *)columnNames { return _columns; }

- (TDBValue *)valueAtIndex:(NSUInteger)index { return _values[index]; }
- (nullable TDBValue *)valueForColumn:(NSString *)name {
    NSNumber *i = _columnIndex[name];
    return i ? _values[i.unsignedIntegerValue] : nil;
}
- (TDBValue *)objectAtIndexedSubscript:(NSUInteger)idx       { return [self valueAtIndex:idx]; }
- (nullable TDBValue *)objectForKeyedSubscript:(NSString *)name { return [self valueForColumn:name]; }

- (NSUInteger)countByEnumeratingWithState:(NSFastEnumerationState *)state
                                  objects:(id  _Nullable __unsafe_unretained *)buffer
                                    count:(NSUInteger)len {
    return [_values countByEnumeratingWithState:state objects:buffer count:len];
}

@end

#pragma mark - TDBResultSet

@implementation TDBResultSet {
    NSArray<TDBRow *> *_rows;
    NSArray<NSString *> *_columns;
}

- (instancetype)initFromNative:(const tdb::sql::ResultSet &)rs {
    self = [super init];
    if (!self) return nil;
    _success = rs.success;
    _errorMessage = rs.error_message.empty() ? nil :
        [NSString stringWithUTF8String:rs.error_message.c_str()];
    _rowsAffected = rs.rows_affected;
    NSMutableArray<NSString *> *cols = [NSMutableArray arrayWithCapacity:rs.columns.size()];
    for (auto &c : rs.columns) [cols addObject:[NSString stringWithUTF8String:c.name.c_str()]];
    _columns = [cols copy];

    NSMutableArray<TDBRow *> *rows = [NSMutableArray arrayWithCapacity:rs.rows.size()];
    for (auto &r : rs.rows) {
        NSMutableArray<TDBValue *> *vals = [NSMutableArray arrayWithCapacity:r.size()];
        for (auto &v : r) [vals addObject:[TDBValue _wrapNative:v]];
        [rows addObject:[[TDBRow alloc] initWithValues:vals columns:_columns]];
    }
    _rows = [rows copy];
    return self;
}

- (NSArray<NSString *> *)columnNames { return _columns; }
- (NSUInteger)rowCount               { return _rows.count; }
- (NSArray<TDBRow *> *)allRows        { return _rows; }

- (TDBRow *)rowAtIndex:(NSUInteger)index           { return _rows[index]; }
- (TDBRow *)objectAtIndexedSubscript:(NSUInteger)idx { return _rows[idx]; }

- (NSUInteger)countByEnumeratingWithState:(NSFastEnumerationState *)state
                                  objects:(id  _Nullable __unsafe_unretained *)buffer
                                    count:(NSUInteger)len {
    return [_rows countByEnumeratingWithState:state objects:buffer count:len];
}

@end
