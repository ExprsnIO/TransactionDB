// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rick Holland
#include "tdb_test.h"
#include "tdb/crypto.h"
#include "tdb/error.h"
#include <cstring>

TDB_TEST(crypto_init_destroy) {
    tdb_crypto_ctx_t ctx;
    std::memset(&ctx, 0, sizeof(ctx));
    uint8_t key[TDB_CRYPTO_KEY_SIZE];
    std::memset(key, 0x42, sizeof(key));
    TDB_REQUIRE_EQ(tdb_crypto_init(&ctx, key, sizeof(key)), TDB_OK);
    TDB_REQUIRE(ctx.initialized);
    tdb_crypto_destroy(&ctx);
}

TDB_TEST(crypto_page_derive_iv) {
    // IV derivation should be deterministic for a given key+page_id.
    tdb_crypto_ctx_t ctx;
    std::memset(&ctx, 0, sizeof(ctx));
    uint8_t key[TDB_CRYPTO_KEY_SIZE];
    std::memset(key, 0xAB, sizeof(key));
    TDB_REQUIRE_EQ(tdb_crypto_init(&ctx, key, sizeof(key)), TDB_OK);

    uint8_t iv1[TDB_CRYPTO_IV_SIZE], iv2[TDB_CRYPTO_IV_SIZE];
    tdb_crypto_derive_page_iv(&ctx, 42, iv1);
    tdb_crypto_derive_page_iv(&ctx, 42, iv2);
    TDB_REQUIRE_EQ(std::memcmp(iv1, iv2, sizeof(iv1)), 0);

    uint8_t iv3[TDB_CRYPTO_IV_SIZE];
    tdb_crypto_derive_page_iv(&ctx, 43, iv3);
    TDB_REQUIRE(std::memcmp(iv1, iv3, sizeof(iv1)) != 0);

    tdb_crypto_destroy(&ctx);
}

TDB_TEST_MAIN("crypto")
