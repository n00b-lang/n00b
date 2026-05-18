/*
 * test_sha512.c — FIPS 180-4 vectors for SHA-512 and SHA-384.
 *
 * Validates the new core/sha512.c implementation before it gets
 * relied on by JWS RS384/RS512 verification (jwt.c).
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/sha512.h"

static int
hex_eq(const uint8_t *got, size_t got_len, const char *expect_hex)
{
    if (strlen(expect_hex) != got_len * 2) return 0;
    for (size_t i = 0; i < got_len; i++) {
        char hi = "0123456789abcdef"[got[i] >> 4];
        char lo = "0123456789abcdef"[got[i] & 0xf];
        if (hi != expect_hex[i * 2] || lo != expect_hex[i * 2 + 1]) return 0;
    }
    return 1;
}

static void
test_sha512_empty(void)
{
    uint8_t out[64];
    n00b_sha512_hash_be("", 0, out);
    static const char *expected =
        "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
        "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e";
    assert(hex_eq(out, 64, expected));
    printf("  [PASS] sha512(\"\")\n");
}

static void
test_sha512_abc(void)
{
    uint8_t out[64];
    n00b_sha512_hash_be("abc", 3, out);
    static const char *expected =
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
        "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f";
    assert(hex_eq(out, 64, expected));
    printf("  [PASS] sha512(\"abc\")\n");
}

static void
test_sha512_two_blocks(void)
{
    /* FIPS 180-2 vector — 112-byte message, two SHA-512 blocks. */
    static const char *msg =
        "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
        "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    uint8_t out[64];
    n00b_sha512_hash_be(msg, strlen(msg), out);
    static const char *expected =
        "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
        "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909";
    assert(hex_eq(out, 64, expected));
    printf("  [PASS] sha512(two-block 112B vector)\n");
}

static void
test_sha384_empty(void)
{
    uint8_t out[48];
    n00b_sha384_hash("", 0, out);
    static const char *expected =
        "38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da"
        "274edebfe76f65fbd51ad2f14898b95b";
    assert(hex_eq(out, 48, expected));
    printf("  [PASS] sha384(\"\")\n");
}

static void
test_sha384_abc(void)
{
    uint8_t out[48];
    n00b_sha384_hash("abc", 3, out);
    static const char *expected =
        "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed"
        "8086072ba1e7cc2358baeca134c825a7";
    assert(hex_eq(out, 48, expected));
    printf("  [PASS] sha384(\"abc\")\n");
}

static void
test_sha512_incremental(void)
{
    /* Same input as test_sha512_abc but fed one byte at a time —
     * exercises the update-state machinery (partial-block buffer,
     * block boundary handling). */
    n00b_sha512_ctx_t ctx;
    n00b_sha512_init(&ctx);
    n00b_sha512_update(&ctx, "a", 1);
    n00b_sha512_update(&ctx, "b", 1);
    n00b_sha512_update(&ctx, "c", 1);
    n00b_sha512_digest_t words;
    n00b_sha512_finalize(&ctx, words);

    uint8_t out[64];
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            out[i * 8 + j] = (uint8_t)(words[i] >> ((7 - j) * 8));
        }
    }
    static const char *expected =
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
        "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f";
    assert(hex_eq(out, 64, expected));
    printf("  [PASS] sha512 incremental update\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("test_sha512:\n");
    fflush(stdout);

    test_sha512_empty();
    test_sha512_abc();
    test_sha512_two_blocks();
    test_sha512_incremental();
    test_sha384_empty();
    test_sha384_abc();

    printf("All sha512 / sha384 tests passed.\n");
    n00b_shutdown();
    return 0;
}
