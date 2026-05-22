//
//  TDBCrypto.mm
//

#import "TDBCrypto.h"
#import <Security/Security.h>
#import "tdb/crypto.h"

#include <cstring>

@implementation TDBCryptoContext {
    NSMutableData *_key; // mutable so we can zero it on dealloc
}

- (instancetype)initWithKey:(NSData *)key {
    NSParameterAssert(key.length == 32);
    self = [super init];
    if (!self) return nil;
    _key = [key mutableCopy];
    return self;
}

- (void)dealloc {
    // Wipe the key on dealloc to limit residue in memory.
    if (_key) {
        memset(_key.mutableBytes, 0, _key.length);
    }
}

+ (NSData *)randomKey {
    uint8_t buf[32];
    int rc = SecRandomCopyBytes(kSecRandomDefault, sizeof(buf), buf);
    NSAssert(rc == errSecSuccess, @"SecRandomCopyBytes failed: %d", rc);
    NSData *out = [NSData dataWithBytes:buf length:sizeof(buf)];
    memset(buf, 0, sizeof(buf));
    return out;
}

+ (NSData *)randomIV {
    uint8_t buf[12];
    int rc = SecRandomCopyBytes(kSecRandomDefault, sizeof(buf), buf);
    NSAssert(rc == errSecSuccess, @"SecRandomCopyBytes failed: %d", rc);
    return [NSData dataWithBytes:buf length:sizeof(buf)];
}

- (nullable NSData *)encrypt:(NSData *)plaintext
                          iv:(NSData *)iv
              additionalData:(nullable NSData *)aad
                        error:(NSError * _Nullable __autoreleasing *)error {
    if (iv.length != 12) {
        if (error) *error = [NSError errorWithDomain:TDBErrorDomain
                                                 code:TDBErrorCodeInvalidArgument
                                             userInfo:@{
            NSLocalizedDescriptionKey: @"GCM IV must be exactly 12 bytes"
        }];
        return nil;
    }
    NSMutableData *out = [NSMutableData dataWithLength:plaintext.length + 16];
    uint8_t tag[16];
    tdb_status_t r = tdb_aes256_gcm_encrypt(
        (const uint8_t *)_key.bytes,
        (const uint8_t *)iv.bytes,
        (const uint8_t *)aad.bytes, aad.length,
        (const uint8_t *)plaintext.bytes, plaintext.length,
        (uint8_t *)out.mutableBytes,
        tag);
    if (r != TDB_OK) {
        if (error) *error = [NSError errorWithDomain:TDBErrorDomain
                                                 code:TDBErrorCodeIO
                                             userInfo:@{
            NSLocalizedDescriptionKey: @"AES-256-GCM encrypt failed"
        }];
        return nil;
    }
    // Append the tag after the ciphertext.
    memcpy((uint8_t *)out.mutableBytes + plaintext.length, tag, 16);
    return [out copy];
}

- (nullable NSData *)decrypt:(NSData *)ctWithTag
                          iv:(NSData *)iv
              additionalData:(nullable NSData *)aad
                        error:(NSError * _Nullable __autoreleasing *)error {
    if (iv.length != 12 || ctWithTag.length < 16) {
        if (error) *error = [NSError errorWithDomain:TDBErrorDomain
                                                 code:TDBErrorCodeInvalidArgument
                                             userInfo:@{
            NSLocalizedDescriptionKey: @"GCM IV must be 12 bytes; ciphertext must include 16-byte tag"
        }];
        return nil;
    }
    size_t ct_len = ctWithTag.length - 16;
    const uint8_t *tag = (const uint8_t *)ctWithTag.bytes + ct_len;
    NSMutableData *out = [NSMutableData dataWithLength:ct_len];
    tdb_status_t r = tdb_aes256_gcm_decrypt(
        (const uint8_t *)_key.bytes,
        (const uint8_t *)iv.bytes,
        (const uint8_t *)aad.bytes, aad.length,
        (const uint8_t *)ctWithTag.bytes, ct_len,
        tag,
        (uint8_t *)out.mutableBytes);
    if (r != TDB_OK) {
        if (error) *error = [NSError errorWithDomain:TDBErrorDomain
                                                 code:TDBErrorCodeIntegrity
                                             userInfo:@{
            NSLocalizedDescriptionKey: @"AES-256-GCM tag verification failed"
        }];
        return nil;
    }
    return [out copy];
}

- (nullable NSData *)sealedEnvelopeForPlaintext:(NSData *)plaintext
                                 additionalData:(nullable NSData *)aad
                                          error:(NSError * _Nullable __autoreleasing *)error {
    NSData *iv = [[self class] randomIV];
    NSData *ct = [self encrypt:plaintext iv:iv additionalData:aad error:error];
    if (!ct) return nil;
    NSMutableData *out = [NSMutableData dataWithCapacity:iv.length + ct.length];
    [out appendData:iv];
    [out appendData:ct];
    return [out copy];
}

- (nullable NSData *)openSealedEnvelope:(NSData *)envelope
                          additionalData:(nullable NSData *)aad
                                    error:(NSError * _Nullable __autoreleasing *)error {
    if (envelope.length < 12 + 16) {
        if (error) *error = [NSError errorWithDomain:TDBErrorDomain
                                                 code:TDBErrorCodeInvalidArgument
                                             userInfo:@{
            NSLocalizedDescriptionKey: @"sealed envelope too short"
        }];
        return nil;
    }
    NSData *iv = [envelope subdataWithRange:NSMakeRange(0, 12)];
    NSData *ct = [envelope subdataWithRange:NSMakeRange(12, envelope.length - 12)];
    return [self decrypt:ct iv:iv additionalData:aad error:error];
}

@end
