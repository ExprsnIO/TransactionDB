//
//  TDBKeychain.h
//  Security.framework Keychain Services wrappers for storing the 32-byte
//  AES-256 master key tied to a TDB database. Keys are stored as
//  `kSecClassGenericPassword` items keyed by `service = "net.rickholland.tdb"`
//  and `account = <caller-provided label>`.
//
//  Access controls (optional):
//    * `accessControl` — pass a SecAccessControlRef built from
//      SecAccessControlCreateWithFlags so the key is only retrievable when
//      the device is unlocked, biometric is satisfied, etc.
//    * `accessGroup` — Keychain access group for app-group sharing.
//

#ifndef TDB_OBJC_KEYCHAIN_H
#define TDB_OBJC_KEYCHAIN_H

#import <Foundation/Foundation.h>
#import <Security/Security.h>
#import "TDBTypes.h"

NS_ASSUME_NONNULL_BEGIN

NS_SWIFT_NAME(TDBKeychain)
@interface TDBKeychain : NSObject

/// Store `key` (32 bytes) in the Keychain. Overwrites any existing item
/// with the same `label`. Returns NO with `error` populated on failure;
/// the OSStatus is in userInfo[TDBErrorOSStatusKey].
+ (BOOL)storeKey:(NSData *)key
        forLabel:(NSString *)label
   accessControl:(nullable SecAccessControlRef)accessControl
     accessGroup:(nullable NSString *)accessGroup
           error:(NSError * _Nullable __autoreleasing * _Nullable)error
    NS_SWIFT_NAME(store(key:label:accessControl:accessGroup:));

/// Retrieve a previously-stored key. Returns nil if no item exists.
+ (nullable NSData *)retrieveKeyForLabel:(NSString *)label
                             accessGroup:(nullable NSString *)accessGroup
                                    error:(NSError * _Nullable __autoreleasing * _Nullable)error
    NS_SWIFT_NAME(retrieveKey(label:accessGroup:));

/// Delete a key. Returns YES even if no item existed (idempotent).
+ (BOOL)deleteKeyForLabel:(NSString *)label
              accessGroup:(nullable NSString *)accessGroup
                    error:(NSError * _Nullable __autoreleasing * _Nullable)error
    NS_SWIFT_NAME(deleteKey(label:accessGroup:));

@end

NS_ASSUME_NONNULL_END

#endif /* TDB_OBJC_KEYCHAIN_H */
