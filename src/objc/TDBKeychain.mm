//
//  TDBKeychain.mm
//

#import "TDBKeychain.h"

static NSString * const kTDBKeychainService = @"net.rickholland.tdb";

static NSError *tdb_make_keychain_error(OSStatus status, NSString *desc) {
    return [NSError errorWithDomain:TDBErrorDomain
                               code:TDBErrorCodeKeychain
                           userInfo:@{
        NSLocalizedDescriptionKey: desc,
        TDBErrorOSStatusKey: @(status),
    }];
}

@implementation TDBKeychain

+ (NSMutableDictionary *)_baseQueryForLabel:(NSString *)label
                                accessGroup:(nullable NSString *)accessGroup {
    NSMutableDictionary *q = [NSMutableDictionary dictionary];
    q[(__bridge id)kSecClass]       = (__bridge id)kSecClassGenericPassword;
    q[(__bridge id)kSecAttrService] = kTDBKeychainService;
    q[(__bridge id)kSecAttrAccount] = label;
    if (accessGroup) q[(__bridge id)kSecAttrAccessGroup] = accessGroup;
    return q;
}

+ (BOOL)storeKey:(NSData *)key
        forLabel:(NSString *)label
   accessControl:(nullable SecAccessControlRef)accessControl
     accessGroup:(nullable NSString *)accessGroup
           error:(NSError * _Nullable __autoreleasing *)error {
    if (key.length == 0) {
        if (error) *error = [NSError errorWithDomain:TDBErrorDomain
                                                 code:TDBErrorCodeInvalidArgument
                                             userInfo:@{NSLocalizedDescriptionKey: @"empty key"}];
        return NO;
    }
    // Delete any existing item first so we get insert-semantics.
    NSMutableDictionary *del_q = [self _baseQueryForLabel:label accessGroup:accessGroup];
    (void)SecItemDelete((__bridge CFDictionaryRef)del_q);

    NSMutableDictionary *add_q = [self _baseQueryForLabel:label accessGroup:accessGroup];
    add_q[(__bridge id)kSecValueData] = key;
    if (accessControl) {
        add_q[(__bridge id)kSecAttrAccessControl] = (__bridge id)accessControl;
    } else {
        add_q[(__bridge id)kSecAttrAccessible] =
            (__bridge id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly;
    }

    OSStatus status = SecItemAdd((__bridge CFDictionaryRef)add_q, NULL);
    if (status != errSecSuccess) {
        if (error) *error = tdb_make_keychain_error(status, @"SecItemAdd failed");
        return NO;
    }
    return YES;
}

+ (nullable NSData *)retrieveKeyForLabel:(NSString *)label
                             accessGroup:(nullable NSString *)accessGroup
                                    error:(NSError * _Nullable __autoreleasing *)error {
    NSMutableDictionary *q = [self _baseQueryForLabel:label accessGroup:accessGroup];
    q[(__bridge id)kSecReturnData]      = @YES;
    q[(__bridge id)kSecMatchLimit]      = (__bridge id)kSecMatchLimitOne;
    CFTypeRef out = NULL;
    OSStatus status = SecItemCopyMatching((__bridge CFDictionaryRef)q, &out);
    if (status == errSecItemNotFound) return nil;
    if (status != errSecSuccess) {
        if (error) *error = tdb_make_keychain_error(status, @"SecItemCopyMatching failed");
        return nil;
    }
    NSData *data = (__bridge_transfer NSData *)out;
    return data;
}

+ (BOOL)deleteKeyForLabel:(NSString *)label
              accessGroup:(nullable NSString *)accessGroup
                    error:(NSError * _Nullable __autoreleasing *)error {
    NSMutableDictionary *q = [self _baseQueryForLabel:label accessGroup:accessGroup];
    OSStatus status = SecItemDelete((__bridge CFDictionaryRef)q);
    if (status == errSecSuccess || status == errSecItemNotFound) return YES;
    if (error) *error = tdb_make_keychain_error(status, @"SecItemDelete failed");
    return NO;
}

@end
