#include "test_unicode_helpers.h"
#include "strings/ansi.h"
#include "core/buffer.h"

// Helper: create a buffer from a C string literal (no NUL terminator).
static n00b_buffer_t *
buf(const char *s, int len)
{
    return n00b_buffer_from_bytes((char *)s, len);
}

// ===================================================================
// Plain text
// ===================================================================

TEST(test_plain_text)
{
    n00b_ansi_ctx *ctx = n00b_ansi_parser_create();
    n00b_ansi_parse(ctx, buf("hello", 5));

    n00b_list_t(n00b_ansi_node_t *) nodes = n00b_ansi_parser_results(ctx);
    size_t len = n00b_list_len(nodes);
    ASSERT(len >= 1);

    n00b_ansi_node_t *n = n00b_list_get(nodes, 0);
    ASSERT_EQ(n->kind, N00B_ANSI_TEXT);
}

// ===================================================================
// SGR (CSI) sequence
// ===================================================================

TEST(test_sgr_sequence)
{
    // \x1b[31m  — Set foreground red
    const char data[] = "\x1b[31m";
    n00b_ansi_ctx *ctx = n00b_ansi_parser_create();
    n00b_ansi_parse(ctx, buf(data, sizeof(data) - 1));

    n00b_list_t(n00b_ansi_node_t *) nodes = n00b_ansi_parser_results(ctx);
    size_t len = n00b_list_len(nodes);
    ASSERT(len >= 1);

    n00b_ansi_node_t *n = n00b_list_get(nodes, 0);
    ASSERT_EQ(n->kind, N00B_ANSI_CONTROL_SEQUENCE);
    ASSERT_EQ(n->ctrl.ctrl_byte, 'm');
}

// ===================================================================
// Mixed text + escape + text
// ===================================================================

TEST(test_mixed)
{
    const char data[] = "abc\x1b[0mxyz";
    n00b_ansi_ctx *ctx = n00b_ansi_parser_create();
    n00b_ansi_parse(ctx, buf(data, sizeof(data) - 1));

    n00b_list_t(n00b_ansi_node_t *) nodes = n00b_ansi_parser_results(ctx);
    size_t len = n00b_list_len(nodes);
    ASSERT_EQ(len, 3);

    // text, control, text
    ASSERT_EQ(n00b_list_get(nodes, 0)->kind, N00B_ANSI_TEXT);
    ASSERT_EQ(n00b_list_get(nodes, 1)->kind, N00B_ANSI_CONTROL_SEQUENCE);
    ASSERT_EQ(n00b_list_get(nodes, 2)->kind, N00B_ANSI_TEXT);
}

// ===================================================================
// C0 control codes
// ===================================================================

TEST(test_c0_bel)
{
    const char data[] = "\x07";
    n00b_ansi_ctx *ctx = n00b_ansi_parser_create();
    n00b_ansi_parse(ctx, buf(data, 1));

    n00b_list_t(n00b_ansi_node_t *) nodes = n00b_ansi_parser_results(ctx);
    size_t len = n00b_list_len(nodes);
    ASSERT(len >= 1);

    n00b_ansi_node_t *n = n00b_list_get(nodes, 0);
    ASSERT_EQ(n->kind, N00B_ANSI_C0_CODE);
    ASSERT_EQ(n->ctrl.ctrl_byte, 0x07);
}

TEST(test_c0_newline)
{
    const char data[] = "\n";
    n00b_ansi_ctx *ctx = n00b_ansi_parser_create();
    n00b_ansi_parse(ctx, buf(data, 1));

    n00b_list_t(n00b_ansi_node_t *) nodes = n00b_ansi_parser_results(ctx);
    size_t len = n00b_list_len(nodes);
    ASSERT(len >= 1);

    n00b_ansi_node_t *n = n00b_list_get(nodes, 0);
    ASSERT_EQ(n->kind, N00B_ANSI_C0_CODE);
    ASSERT_EQ(n->ctrl.ctrl_byte, '\n');
}

// ===================================================================
// nodes_to_string with keep_control=true
// ===================================================================

TEST(test_to_string_keep)
{
    const char data[] = "hi\x1b[1mthere";
    n00b_ansi_ctx *ctx = n00b_ansi_parser_create();
    n00b_ansi_parse(ctx, buf(data, sizeof(data) - 1));

    n00b_list_t(n00b_ansi_node_t *) nodes = n00b_ansi_parser_results(ctx);
    n00b_string_t s = n00b_ansi_nodes_to_string(nodes, true);

    // Should reconstruct the full input
    ASSERT(s.u8_bytes > 0);
    // Must contain the escape sequence
    ASSERT(memchr(s.data, '\x1b', s.u8_bytes) != nullptr);
}

// ===================================================================
// nodes_to_string with keep_control=false
// ===================================================================

TEST(test_to_string_strip)
{
    const char data[] = "hi\x1b[1mthere";
    n00b_ansi_ctx *ctx = n00b_ansi_parser_create();
    n00b_ansi_parse(ctx, buf(data, sizeof(data) - 1));

    n00b_list_t(n00b_ansi_node_t *) nodes = n00b_ansi_parser_results(ctx);
    n00b_string_t s = n00b_ansi_nodes_to_string(nodes, false);

    // Should strip the escape sequence, leaving only text
    ASSERT_STR_EQ(s.data, "hithere");
}

// ===================================================================
// Newlines preserved when stripping
// ===================================================================

TEST(test_strip_keeps_newline)
{
    const char data[] = "a\n\x1b[31mb";
    n00b_ansi_ctx *ctx = n00b_ansi_parser_create();
    n00b_ansi_parse(ctx, buf(data, sizeof(data) - 1));

    n00b_list_t(n00b_ansi_node_t *) nodes = n00b_ansi_parser_results(ctx);
    n00b_string_t s = n00b_ansi_nodes_to_string(nodes, false);

    ASSERT_STR_EQ(s.data, "a\nb");
}

// ===================================================================
// Fe sequence (e.g., ESC D = Index)
// ===================================================================

TEST(test_fe_sequence)
{
    const char data[] = "\x1b" "D";
    n00b_ansi_ctx *ctx = n00b_ansi_parser_create();
    n00b_ansi_parse(ctx, buf(data, 2));

    n00b_list_t(n00b_ansi_node_t *) nodes = n00b_ansi_parser_results(ctx);
    size_t len = n00b_list_len(nodes);
    ASSERT(len >= 1);

    n00b_ansi_node_t *n = n00b_list_get(nodes, 0);
    ASSERT_EQ(n->kind, N00B_ANSI_FE_SEQUENCE);
}

// ===================================================================
// Multiple CSI sequences
// ===================================================================

TEST(test_multiple_csi)
{
    // Two SGR sequences back to back
    const char data[] = "\x1b[1m\x1b[31m";
    n00b_ansi_ctx *ctx = n00b_ansi_parser_create();
    n00b_ansi_parse(ctx, buf(data, sizeof(data) - 1));

    n00b_list_t(n00b_ansi_node_t *) nodes = n00b_ansi_parser_results(ctx);
    size_t len = n00b_list_len(nodes);
    ASSERT_EQ(len, 2);

    ASSERT_EQ(n00b_list_get(nodes, 0)->kind, N00B_ANSI_CONTROL_SEQUENCE);
    ASSERT_EQ(n00b_list_get(nodes, 1)->kind, N00B_ANSI_CONTROL_SEQUENCE);
}

// ===================================================================
// Empty input
// ===================================================================

TEST(test_empty_input)
{
    n00b_ansi_ctx *ctx = n00b_ansi_parser_create();
    n00b_ansi_parse(ctx, buf("", 0));

    n00b_list_t(n00b_ansi_node_t *) nodes = n00b_ansi_parser_results(ctx);
    size_t len = n00b_list_len(nodes);
    ASSERT_EQ(len, 0);
}

static void run_tests(void)
{
    RUN_TEST(test_plain_text);
    RUN_TEST(test_sgr_sequence);
    RUN_TEST(test_mixed);
    RUN_TEST(test_c0_bel);
    RUN_TEST(test_c0_newline);
    RUN_TEST(test_to_string_keep);
    RUN_TEST(test_to_string_strip);
    RUN_TEST(test_strip_keeps_newline);
    RUN_TEST(test_fe_sequence);
    RUN_TEST(test_multiple_csi);
    RUN_TEST(test_empty_input);
}

TEST_MAIN()
