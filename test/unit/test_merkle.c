// test_merkle.c — WP-001 Phase 3: content-addressed Merkle DAG.
//
// Covers: content addressing + dedup (identical payload -> identical hash, no
// second copy), distinct payloads -> distinct hashes, links bound by child
// node_hash with round-trip via get(), payload marshal round-trip, missing-node
// lookups, link-to-missing-node rejection (acyclicity), and cross-DAG
// determinism (same content -> same hash in independent DAGs).

#include <stdint.h>

#include "n00b.h"
#include "crypto/merkle.h"
#include "core/runtime.h"
#include "util/assert.h"

#define CHECK(e)                                                               \
    do {                                                                       \
        n00b_require((e), "merkle test: " #e);                                 \
    } while (0)

typedef struct mk_point_t {
    int64_t x;
    int64_t y;
} mk_point_t;

static mk_point_t *
mk_pt(int64_t x, int64_t y)
{
    mk_point_t *p = n00b_alloc(mk_point_t);
    p->x          = x;
    p->y          = y;
    return p;
}

static n00b_merkle_hash_t *
hash_box(n00b_merkle_hash_t h)
{
    n00b_merkle_hash_t *p = n00b_alloc(n00b_merkle_hash_t);
    *p                    = h;
    return p;
}

static void
test_merkle(void)
{
    n00b_merkle_t *dag = n00b_merkle_new(mk_point_t);
    CHECK(n00b_merkle_count(dag) == 0);

    auto r1 = n00b_merkle_add(dag, mk_point_t, mk_pt(1, 2), nullptr);
    CHECK(n00b_result_is_ok(r1));
    n00b_merkle_hash_t h1 = n00b_result_get(r1);
    CHECK(n00b_merkle_count(dag) == 1);
    CHECK(n00b_merkle_contains(dag, &h1));

    // Dedup: identical payload -> same node_hash, count unchanged.
    auto r1b = n00b_merkle_add(dag, mk_point_t, mk_pt(1, 2), nullptr);
    CHECK(n00b_result_is_ok(r1b));
    n00b_merkle_hash_t h1b = n00b_result_get(r1b);
    CHECK(n00b_merkle_hash_eq(&h1, &h1b));
    CHECK(n00b_merkle_count(dag) == 1);

    // Distinct payload -> distinct hash.
    auto r2 = n00b_merkle_add(dag, mk_point_t, mk_pt(3, 4), nullptr);
    CHECK(n00b_result_is_ok(r2));
    n00b_merkle_hash_t h2 = n00b_result_get(r2);
    CHECK(!n00b_merkle_hash_eq(&h1, &h2));
    CHECK(n00b_merkle_count(dag) == 2);

    // Parent linking [h1, h2].
    n00b_list_t(n00b_merkle_hash_t *) links
        = n00b_list_new(n00b_merkle_hash_t *);
    n00b_list_push(links, hash_box(h1));
    n00b_list_push(links, hash_box(h2));
    auto r3 = n00b_merkle_add(dag, mk_point_t, mk_pt(5, 6), &links);
    CHECK(n00b_result_is_ok(r3));
    n00b_merkle_hash_t h3 = n00b_result_get(r3);
    CHECK(n00b_merkle_count(dag) == 3);

    // get(h3): ordered links round-trip by child node_hash.
    auto on = n00b_merkle_get(dag, &h3);
    CHECK(n00b_option_is_set(on));
    n00b_merkle_node_t *node = n00b_option_get(on);
    CHECK(node->link_count == 2);
    CHECK(n00b_merkle_hash_eq(&node->links[0], &h1));
    CHECK(n00b_merkle_hash_eq(&node->links[1], &h2));

    // Payload marshal round-trip on h1.
    auto o1 = n00b_merkle_get(dag, &h1);
    CHECK(n00b_option_is_set(o1));
    mk_point_t *p1 = n00b_merkle_payload(mk_point_t, dag, n00b_option_get(o1));
    CHECK(p1 != nullptr);
    CHECK(p1->x == 1 && p1->y == 2);

    // Unknown node -> not contained, get is none.
    n00b_merkle_hash_t bogus = {.alg = N00B_MERKLE_SHA256, .len = 32};
    bogus.bytes[0]           = 0xab;
    CHECK(!n00b_merkle_contains(dag, &bogus));
    auto omiss = n00b_merkle_get(dag, &bogus);
    CHECK(!n00b_option_is_set(omiss));

    // Linking a node that isn't in the DAG is rejected (keeps it acyclic).
    n00b_list_t(n00b_merkle_hash_t *) bad = n00b_list_new(n00b_merkle_hash_t *);
    n00b_list_push(bad, hash_box(bogus));
    auto rbad = n00b_merkle_add(dag, mk_point_t, mk_pt(7, 8), &bad);
    CHECK(n00b_result_is_err(rbad));
    CHECK(n00b_result_get_err(rbad) == N00B_MERKLE_ERR_LINK_MISSING);
}

static void
test_determinism(void)
{
    // Independent DAGs, identical content -> identical node hash.
    n00b_merkle_t *d1 = n00b_merkle_new(mk_point_t);
    n00b_merkle_t *d2 = n00b_merkle_new(mk_point_t);

    auto ra = n00b_merkle_add(d1, mk_point_t, mk_pt(9, 9), nullptr);
    auto rb = n00b_merkle_add(d2, mk_point_t, mk_pt(9, 9), nullptr);
    CHECK(n00b_result_is_ok(ra) && n00b_result_is_ok(rb));
    n00b_merkle_hash_t a = n00b_result_get(ra);
    n00b_merkle_hash_t b = n00b_result_get(rb);
    CHECK(n00b_merkle_hash_eq(&a, &b));
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    test_merkle();
    test_determinism();

    n00b_shutdown();
    return 0;
}
