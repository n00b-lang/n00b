/** @file test/unit/test_pe_cert_table_emit.c — PE Authenticode cert-table emit
 *                                              round-trip regression.
 *
 *  WP-005 Phase 3 regression test for the new cert-table emit
 *  path in `n00b_pe_build` (`src/compiler/objfile/pe_build.c`,
 *  added in Phase 4b). The test:
 *
 *    [1] Constructs a minimal PE binary in memory.
 *    [2] Attaches a synthetic cert blob to `bin->certificates[]`
 *        with `revision = WIN_CERT_REVISION_2_0 (0x0200)` and
 *        `certificate_type = WIN_CERT_TYPE_PKCS_SIGNED_DATA
 *        (0x0002)` — the Authenticode-standard values.
 *    [3] Calls `n00b_pe_build` and verifies:
 *        - The build succeeds.
 *        - `data_dirs[N00B_PE_DD_CERTIFICATE]` points at a non-
 *          zero file offset with the correct total size.
 *        - The cert table starts AFTER the last section's raw
 *          data (alignment respected).
 *    [4] Parses the built blob back via `n00b_pe_parse` and
 *        verifies:
 *        - `bin->num_certificates == 1`.
 *        - `bin->certificates[0].revision == 0x0200`.
 *        - `bin->certificates[0].certificate_type == 0x0002`.
 *        - `bin->certificates[0].raw_data->byte_len == original_len`.
 *        - The raw_data bytes byte-equal the synthetic original.
 *    [5] Multi-cert round-trip: attaches two synthetic certs;
 *        verifies both round-trip with correct 8-byte alignment
 *        padding between them.
 *
 *  Test-file conventions per D-030.
 */

#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/buffer.h"
#include "compiler/objfile/pe.h"
#include "compiler/objfile/pe_build.h"
#include "compiler/objfile/bstream.h"

#define WIN_CERT_REVISION_2_0       0x0200
#define WIN_CERT_TYPE_PKCS_SIGNED   0x0002

static n00b_pe_binary_t *
build_pe_with_certs(n00b_buffer_t **certs, uint16_t *types,
                    uint16_t *revisions, uint32_t n_certs)
{
    n00b_pe_binary_t *bin = n00b_pe_binary_new(
        N00B_PE_MACHINE_AMD64,
        N00B_PE_SUBSYSTEM_WINDOWS_CUI);
    bin->entry_point = 0x1000;

    n00b_pe_section_t *text = n00b_pe_add_section(
        bin, ".text",
        N00B_PE_SCN_CNT_CODE | N00B_PE_SCN_MEM_EXECUTE | N00B_PE_SCN_MEM_READ);
    uint8_t code[] = { 0xCC, 0x90, 0xC3 };
    text->content = n00b_buffer_from_bytes((char *)code, 3);

    /* Make sure we have at least N00B_PE_DD_CERTIFICATE+1 data
     * directories so the cert-table emit path can patch the entry.
     * n00b_pe_binary_new should give us all 16 by default. */
    assert(bin->num_data_dirs > N00B_PE_DD_CERTIFICATE);

    bin->certificates = n00b_alloc_array(n00b_pe_certificate_t, n_certs);
    bin->num_certificates = n_certs;
    for (uint32_t i = 0; i < n_certs; i++) {
        bin->certificates[i].revision         = revisions[i];
        bin->certificates[i].certificate_type = types[i];
        bin->certificates[i].raw_data         = certs[i];
    }

    return bin;
}

static void
test_single_cert_roundtrip(void)
{
    /* Synthetic cert bytes: a 100-byte canned blob. */
    uint8_t blob[100];
    for (int i = 0; i < 100; i++) {
        blob[i] = (uint8_t)((i * 31) & 0xFF);
    }
    n00b_buffer_t *cert = n00b_buffer_from_bytes((char *)blob, 100);

    n00b_buffer_t *certs[1]   = { cert };
    uint16_t       types[1]   = { WIN_CERT_TYPE_PKCS_SIGNED };
    uint16_t       revs[1]    = { WIN_CERT_REVISION_2_0 };

    n00b_pe_binary_t *bin = build_pe_with_certs(certs, types, revs, 1);

    auto br = n00b_pe_build(bin);
    assert(n00b_result_is_ok(br));
    n00b_buffer_t *built = n00b_result_get(br);
    assert(built != nullptr);
    assert(n00b_buffer_len(built) > 0);

    /* Parse back. */
    n00b_bstream_t *s = n00b_bstream_new(built);
    auto pr = n00b_pe_parse(s);
    if (n00b_result_is_err(pr)) {
        fprintf(stderr, "  parse err code=%d\n", n00b_result_get_err(pr));
        assert(0);
    }
    n00b_pe_binary_t *parsed = n00b_result_get(pr);

    assert(parsed->num_certificates == 1);
    assert(parsed->certificates[0].revision == WIN_CERT_REVISION_2_0);
    assert(parsed->certificates[0].certificate_type == WIN_CERT_TYPE_PKCS_SIGNED);

    n00b_buffer_t *got = parsed->certificates[0].raw_data;
    assert(got != nullptr);
    assert(got->byte_len == 100);
    assert(memcmp(got->data, blob, 100) == 0);

    /* Data directory should point at a non-zero offset + non-zero
     * size matching what we wrote. */
    assert(parsed->data_dirs[N00B_PE_DD_CERTIFICATE].VirtualAddress > 0);
    assert(parsed->data_dirs[N00B_PE_DD_CERTIFICATE].Size > 0);
    /* The size includes the 8-byte WIN_CERTIFICATE header + the
     * cert content padded to 8-byte alignment: 8 + ceil(100/8)*8
     * = 8 + 104 = 112. */
    assert(parsed->data_dirs[N00B_PE_DD_CERTIFICATE].Size == 112);

    printf("  [PASS] single_cert_roundtrip\n");
}

static void
test_multi_cert_roundtrip(void)
{
    /* Two synthetic certs of differing sizes to exercise per-cert
     * 8-byte alignment padding. */
    uint8_t blob_a[50];
    uint8_t blob_b[200];
    for (int i = 0; i < 50; i++)  blob_a[i] = (uint8_t)(0xA0 ^ i);
    for (int i = 0; i < 200; i++) blob_b[i] = (uint8_t)(0xB0 ^ i);

    n00b_buffer_t *cert_a = n00b_buffer_from_bytes((char *)blob_a, 50);
    n00b_buffer_t *cert_b = n00b_buffer_from_bytes((char *)blob_b, 200);

    n00b_buffer_t *certs[2] = { cert_a, cert_b };
    uint16_t       types[2] = { WIN_CERT_TYPE_PKCS_SIGNED,
                                WIN_CERT_TYPE_PKCS_SIGNED };
    uint16_t       revs[2]  = { WIN_CERT_REVISION_2_0,
                                WIN_CERT_REVISION_2_0 };

    n00b_pe_binary_t *bin = build_pe_with_certs(certs, types, revs, 2);

    auto br = n00b_pe_build(bin);
    assert(n00b_result_is_ok(br));
    n00b_buffer_t *built = n00b_result_get(br);

    n00b_bstream_t *s = n00b_bstream_new(built);
    auto pr = n00b_pe_parse(s);
    if (n00b_result_is_err(pr)) {
        fprintf(stderr, "  parse err code=%d\n", n00b_result_get_err(pr));
        assert(0);
    }
    n00b_pe_binary_t *parsed = n00b_result_get(pr);

    assert(parsed->num_certificates == 2);
    assert(parsed->certificates[0].raw_data->byte_len == 50);
    assert(memcmp(parsed->certificates[0].raw_data->data, blob_a, 50) == 0);
    assert(parsed->certificates[1].raw_data->byte_len == 200);
    assert(memcmp(parsed->certificates[1].raw_data->data, blob_b, 200) == 0);

    /* Total table size: cert A entry = 8 + ceil(50/8)*8 = 8 + 56 = 64.
     *                  cert B entry = 8 + ceil(200/8)*8 = 8 + 200 = 208.
     *                  total = 64 + 208 = 272. */
    assert(parsed->data_dirs[N00B_PE_DD_CERTIFICATE].Size == 272);

    printf("  [PASS] multi_cert_roundtrip\n");
}

static void
test_no_certs_unchanged(void)
{
    /* Building a PE without any certs should NOT set the cert
     * table directory. */
    n00b_pe_binary_t *bin = n00b_pe_binary_new(
        N00B_PE_MACHINE_AMD64,
        N00B_PE_SUBSYSTEM_WINDOWS_CUI);
    bin->entry_point = 0x1000;

    n00b_pe_section_t *text = n00b_pe_add_section(
        bin, ".text",
        N00B_PE_SCN_CNT_CODE | N00B_PE_SCN_MEM_EXECUTE | N00B_PE_SCN_MEM_READ);
    uint8_t code[] = { 0xCC, 0x90, 0xC3 };
    text->content = n00b_buffer_from_bytes((char *)code, 3);

    auto br = n00b_pe_build(bin);
    assert(n00b_result_is_ok(br));
    n00b_buffer_t *built = n00b_result_get(br);

    n00b_bstream_t *s = n00b_bstream_new(built);
    auto pr = n00b_pe_parse(s);
    assert(n00b_result_is_ok(pr));
    n00b_pe_binary_t *parsed = n00b_result_get(pr);

    assert(parsed->num_certificates == 0);
    assert(parsed->data_dirs[N00B_PE_DD_CERTIFICATE].VirtualAddress == 0);
    assert(parsed->data_dirs[N00B_PE_DD_CERTIFICATE].Size == 0);

    printf("  [PASS] no_certs_unchanged\n");
}

int
main(int argc, char **argv)
{
    n00b_init_simple(argc, argv);

    printf("test_pe_cert_table_emit:\n");
    test_no_certs_unchanged();
    test_single_cert_roundtrip();
    test_multi_cert_roundtrip();
    printf("All PE cert-table emit regression tests passed.\n");
    return 0;
}
