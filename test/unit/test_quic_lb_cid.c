/*
 * test_quic_lb_cid.c — LB-CID block-cipher-mode round-trip tests.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "n00b.h"
#include "core/runtime.h"
#include "net/quic/quic_types.h"
#include "net/quic/lb_cid.h"

/* ============================================================================
 * 1. encode → decode round-trip across server-id values + lengths
 * ============================================================================ */

static void
test_encode_decode_roundtrip(void)
{
    static const uint8_t key[16] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
    };

    /* Try a handful of server-id values + lengths. */
    struct {
        uint64_t sid;
        uint8_t  len;
    } cases[] = {
        {        0x42ULL, 1},
        {     0xabcdULL, 2},
        {  0xdeadbeefULL, 4},
        {0x1234567890abULL, 6},
        {                0xfeULL, 1},
        {              0x00ffULL, 2},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        auto cr = n00b_quic_lb_cid_config_new(key, cases[i].sid, cases[i].len);
        assert(n00b_result_is_ok(cr));
        n00b_quic_lb_cid_config_t *cfg = n00b_result_get(cr);

        uint8_t encoded[N00B_QUIC_LB_CID_LEN];
        auto er = n00b_quic_lb_cid_encode(cfg, encoded);
        assert(n00b_result_is_ok(er));

        uint64_t got = 0;
        auto dr = n00b_quic_lb_cid_decode(cfg, encoded, &got);
        assert(n00b_result_is_ok(dr));

        /* Mask the input to the expected number of low bytes — the
         * encoder uses only `server_id_len` low bytes. */
        uint64_t mask = (cases[i].len >= 8)
                        ? ~0ULL
                        : ((1ULL << (cases[i].len * 8)) - 1ULL);
        assert(got == (cases[i].sid & mask));

        n00b_quic_lb_cid_config_close(cfg);
    }
    printf("  [PASS] encode/decode round-trip across 6 server-id shapes\n");
}

/* ============================================================================
 * 2. Same server-id encodes differently each time (fresh nonce).
 * ============================================================================ */

static void
test_nonce_freshness(void)
{
    static const uint8_t key[16] = {0};
    auto cr = n00b_quic_lb_cid_config_new(key, 0xcafe, 2);
    n00b_quic_lb_cid_config_t *cfg = n00b_result_get(cr);

    uint8_t a[16], b[16], c[16];
    n00b_quic_lb_cid_encode(cfg, a);
    n00b_quic_lb_cid_encode(cfg, b);
    n00b_quic_lb_cid_encode(cfg, c);

    /* All three pairs differ. */
    assert(memcmp(a, b, 16) != 0);
    assert(memcmp(b, c, 16) != 0);
    assert(memcmp(a, c, 16) != 0);

    /* But each decodes to the same server-id. */
    uint64_t sa, sb, sc;
    n00b_quic_lb_cid_decode(cfg, a, &sa);
    n00b_quic_lb_cid_decode(cfg, b, &sb);
    n00b_quic_lb_cid_decode(cfg, c, &sc);
    assert(sa == 0xcafe && sb == 0xcafe && sc == 0xcafe);

    n00b_quic_lb_cid_config_close(cfg);
    printf("  [PASS] same server-id encodes uniquely (fresh nonce per call)\n");
}

/* ============================================================================
 * 3. Different server-ids encode to different CIDs (key + sid drive output).
 * ============================================================================ */

static void
test_sid_routing(void)
{
    static const uint8_t key[16] = {0};

    auto c1 = n00b_quic_lb_cid_config_new(key, 0x01, 1);
    auto c2 = n00b_quic_lb_cid_config_new(key, 0x02, 1);
    n00b_quic_lb_cid_config_t *cfg1 = n00b_result_get(c1);
    n00b_quic_lb_cid_config_t *cfg2 = n00b_result_get(c2);

    uint8_t e1[16], e2[16];
    n00b_quic_lb_cid_encode(cfg1, e1);
    n00b_quic_lb_cid_encode(cfg2, e2);

    /* The LB needs to be able to tell instances apart from the
     * encrypted CID alone.  Decoding e1 with cfg1 yields 1; with
     * cfg2 also yields some value (decryption with the same key
     * just runs AES-1) but the meaningful bytes differ. */
    uint64_t s1_via_1, s2_via_2;
    n00b_quic_lb_cid_decode(cfg1, e1, &s1_via_1);
    n00b_quic_lb_cid_decode(cfg2, e2, &s2_via_2);
    assert(s1_via_1 == 1);
    assert(s2_via_2 == 2);

    /* And cross-decode (same key) still works since both ECB
     * sessions share the key. */
    uint64_t s1_via_2;
    n00b_quic_lb_cid_decode(cfg2, e1, &s1_via_2);
    assert(s1_via_2 == 1);

    n00b_quic_lb_cid_config_close(cfg1);
    n00b_quic_lb_cid_config_close(cfg2);
    printf("  [PASS] different server-ids decode correctly\n");
}

/* ============================================================================
 * 4. Argument validation
 * ============================================================================ */

static void
test_arg_validation(void)
{
    static const uint8_t key[16] = {0};

    /* NULL key. */
    assert(n00b_result_is_err(n00b_quic_lb_cid_config_new(NULL, 1, 1)));
    /* server_id_len 0 / 16 / 17. */
    assert(n00b_result_is_err(n00b_quic_lb_cid_config_new(key, 1, 0)));
    assert(n00b_result_is_err(n00b_quic_lb_cid_config_new(key, 1, 16)));
    assert(n00b_result_is_err(n00b_quic_lb_cid_config_new(key, 1, 17)));

    /* close() is idempotent + null-safe. */
    n00b_quic_lb_cid_config_close(NULL);
    auto r = n00b_quic_lb_cid_config_new(key, 1, 1);
    n00b_quic_lb_cid_config_t *cfg = n00b_result_get(r);
    n00b_quic_lb_cid_config_close(cfg);
    n00b_quic_lb_cid_config_close(cfg);

    /* encode/decode on closed config → error. */
    uint8_t e[16];
    uint64_t s;
    assert(n00b_result_is_err(n00b_quic_lb_cid_encode(cfg, e)));
    assert(n00b_result_is_err(n00b_quic_lb_cid_decode(cfg, e, &s)));
    printf("  [PASS] argument validation + close-safety\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("test_quic_lb_cid:\n");
    test_encode_decode_roundtrip();
    test_nonce_freshness();
    test_sid_routing();
    test_arg_validation();
    printf("All quic_lb_cid tests passed.\n");

    n00b_shutdown();
    return 0;
}
