//
//  TDBCrypto.h
//  AES-256-GCM bindings over the in-tree pure-C primitive
//  (tdb_aes256_gcm_encrypt/decrypt), with Security.framework for nonce
//  generation. The crypto context is an Objective-C class — TDBCryptoContext
//  — that owns the 32-byte key, optionally backed by the Keychain via
//  TDBKeychain so the key never lives in plaintext NSData longer than
//  necessary.
//

#ifndef TDB_OBJC_CRYPTO_H
#define TDB_OBJC_CRYPTO_H

#import <Foundation/Foundation.h>
#import "TDBTypes.h"

NS_ASSUME_NONNULL_BEGIN

@protocol OS_tdb_crypto_context <NSObject>
@end

NS_SWIFT_NAME(TDBCryptoContext)
@interface TDBCryptoContext : NSObject <OS_tdb_crypto_context>

/// 32-byte key for AES-256. The class clears its copy on dealloc.
- (instancetype)initWithKey:(NSData *)key NS_DESIGNATED_INITIALIZER;

/// Generate a fresh 32-byte AES key via SecRandomCopyBytes.
+ (NSData *)randomKey;

/// Generate a fresh 12-byte (96-bit) GCM IV via SecRandomCopyBytes.
+ (NSData *)randomIV;

/// Encrypt under AES-256-GCM. `iv` MUST be 12 bytes. The returned data is
/// the ciphertext concatenated with the 16-byte tag.
- (nullable NSData *)encrypt:(NSData *)plaintext
                          iv:(NSData *)iv
              additionalData:(nullable NSData *)aad
                        error:(NSError * _Nullable __autoreleasing * _Nullable)error
    NS_SWIFT_NAME(encrypt(plaintext:iv:additionalData:));

/// Decrypt previously-encrypted bytes (ciphertext || 16-byte tag). Returns
/// nil with `error` populated on tag mismatch — i.e. tampering detected.
- (nullable NSData *)decrypt:(NSData *)ciphertextWithTag
                          iv:(NSData *)iv
              additionalData:(nullable NSData *)aad
                        error:(NSError * _Nullable __autoreleasing * _Nullable)error
    NS_SWIFT_NAME(decrypt(_:iv:additionalData:));

/// Convenience: encrypt and prepend the IV to the output (12-byte IV
/// followed by ciphertext-with-tag). Useful for stream-style envelopes.
- (nullable NSData *)sealedEnvelopeForPlaintext:(NSData *)plaintext
                                 additionalData:(nullable NSData *)aad
                                          error:(NSError * _Nullable __autoreleasing * _Nullable)error;

/// Inverse of `sealedEnvelopeForPlaintext:`.
- (nullable NSData *)openSealedEnvelope:(NSData *)envelope
                          additionalData:(nullable NSData *)aad
                                    error:(NSError * _Nullable __autoreleasing * _Nullable)error;

- (instancetype)init NS_UNAVAILABLE;

@end

NS_ASSUME_NONNULL_END

#endif /* TDB_OBJC_CRYPTO_H */
