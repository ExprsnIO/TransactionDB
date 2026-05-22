//
//  TDBResultSet.h
//  Result of a SELECT (or any statement that returns rows). Provides
//  array-like access plus NSFastEnumeration so `for (TDBValue *v in row)`
//  works directly. Backed by tdb::sql::ResultSet via an internal helper.
//

#ifndef TDB_OBJC_RESULT_SET_H
#define TDB_OBJC_RESULT_SET_H

#import <Foundation/Foundation.h>
#import "TDBTypes.h"
#import "TDBValue.h"

NS_ASSUME_NONNULL_BEGIN

@protocol OS_tdb_result_set <NSObject>
@end

/// Row in a result set — an immutable, indexable list of TDBValue.
NS_SWIFT_NAME(TDBRow)
@interface TDBRow : NSObject <NSFastEnumeration>
@property (nonatomic, copy, readonly) NSArray<TDBValue *> *values;
@property (nonatomic, copy, readonly) NSArray<NSString *> *columnNames;
- (TDBValue *)valueAtIndex:(NSUInteger)index;
- (nullable TDBValue *)valueForColumn:(NSString *)name;
- (TDBValue *)objectAtIndexedSubscript:(NSUInteger)idx;
- (nullable TDBValue *)objectForKeyedSubscript:(NSString *)name;
@end

NS_SWIFT_NAME(TDBResultSet)
@interface TDBResultSet : NSObject <OS_tdb_result_set, NSFastEnumeration>

@property (nonatomic, readonly) BOOL success;
@property (nonatomic, copy, readonly, nullable) NSString *errorMessage;
@property (nonatomic, readonly) int64_t rowsAffected;
@property (nonatomic, copy, readonly) NSArray<NSString *> *columnNames;
@property (nonatomic, readonly) NSUInteger rowCount;

/// Access by row index, then by column index or name.
- (TDBRow *)rowAtIndex:(NSUInteger)index;
- (TDBRow *)objectAtIndexedSubscript:(NSUInteger)idx;

/// All rows materialized at once. Use NSFastEnumeration for large sets.
- (NSArray<TDBRow *> *)allRows;

@end

NS_ASSUME_NONNULL_END

#endif /* TDB_OBJC_RESULT_SET_H */
