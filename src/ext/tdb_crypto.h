/*
** tdb_crypto.h — built-in cryptography extension.
**
** Registers a suite of SQL scalar functions on a connection using the same
** public plugin API a third-party C/C++ extension would use
** (tdb_create_function). Hashing (sha256, sha1, md5, hmac_sha256), CRC32 and
** hex/unhex/randomblob are implemented in-tree so they work in offline builds;
** when the library is configured with -DTDB_BUILD_OPENSSL=ON (TDB_HAVE_OPENSSL)
** symmetric encryption (aes_encrypt/aes_decrypt) and a CSPRNG are added on top.
*/
#ifndef TDB_CRYPTO_H
#define TDB_CRYPTO_H

#include "transactiondb.h"

/* Register the built-in cryptographic functions on `db`. Always succeeds. */
int tdb_register_crypto(tdb_db *db);

#endif /* TDB_CRYPTO_H */
