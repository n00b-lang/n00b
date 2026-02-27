#include "test_unicode_helpers.h"
#include "strings/markdown.h"

// Helper: md4c wraps all content in a BLOCK_BODY (MD_BLOCK_DOC).
// This returns the first BLOCK_BODY child of root, or root itself
// if the structure is unexpected.
static n00b_tree_t(n00b_md_node_t, n00b_md_node_t) *
get_body(n00b_tree_t(n00b_md_node_t, n00b_md_node_t) *root)
{
    for (size_t i = 0; i < n00b_tree_num_children(root); i++) {
        auto child = n00b_tree_child(root, i);
        if (n00b_tree_node_value(child).node_type == N00B_MD_BLOCK_BODY) {
            return child;
        }
    }
    return root;
}

// Recursive search for a node kind anywhere in the tree.
static bool
find_kind(n00b_tree_t(n00b_md_node_t, n00b_md_node_t) *t,
          n00b_md_node_kind_t target)
{
    if (n00b_tree_node_value(t).node_type == target) {
        return true;
    }
    for (size_t i = 0; i < n00b_tree_num_children(t); i++) {
        if (find_kind(n00b_tree_child(t, i), target)) {
            return true;
        }
    }
    return false;
}

// ===================================================================
// Empty input
// ===================================================================

TEST(test_empty)
{
    n00b_string_t s = n00b_string_from_raw("", 0);

    auto root = n00b_parse_markdown(s);
    ASSERT(root != nullptr);
    ASSERT_EQ(n00b_tree_node_value(root).node_type, N00B_MD_DOCUMENT);
}

// ===================================================================
// Single paragraph
// ===================================================================

TEST(test_paragraph)
{
    n00b_string_t s = n00b_string_from_cstr("Hello world");

    auto root = n00b_parse_markdown(s);
    ASSERT(root != nullptr);

    auto body = get_body(root);
    ASSERT(n00b_tree_num_children(body) >= 1);

    // Should contain a P block
    ASSERT(find_kind(body, N00B_MD_BLOCK_P));
}

// ===================================================================
// Heading
// ===================================================================

TEST(test_heading)
{
    n00b_string_t s = n00b_string_from_cstr("# Title");

    auto root = n00b_parse_markdown(s);
    ASSERT(root != nullptr);

    ASSERT(find_kind(root, N00B_MD_BLOCK_H));
}

// ===================================================================
// Bold text
// ===================================================================

TEST(test_bold)
{
    n00b_string_t s = n00b_string_from_cstr("**bold**");

    auto root = n00b_parse_markdown(s);
    ASSERT(root != nullptr);

    ASSERT(find_kind(root, N00B_MD_SPAN_STRONG));
}

// ===================================================================
// Italic text
// ===================================================================

TEST(test_italic)
{
    n00b_string_t s = n00b_string_from_cstr("*emphasis*");

    auto root = n00b_parse_markdown(s);
    ASSERT(root != nullptr);

    ASSERT(find_kind(root, N00B_MD_SPAN_EM));
}

// ===================================================================
// Code block
// ===================================================================

TEST(test_code_block)
{
    n00b_string_t s = n00b_string_from_cstr("```\ncode\n```");

    auto root = n00b_parse_markdown(s);
    ASSERT(root != nullptr);

    ASSERT(find_kind(root, N00B_MD_BLOCK_CODE));
}

// ===================================================================
// Unordered list
// ===================================================================

TEST(test_unordered_list)
{
    n00b_string_t s = n00b_string_from_cstr("- item1\n- item2\n- item3");

    auto root = n00b_parse_markdown(s);
    ASSERT(root != nullptr);

    auto body = get_body(root);

    // Find the UL block
    bool found_ul = false;
    for (size_t i = 0; i < n00b_tree_num_children(body); i++) {
        auto child = n00b_tree_child(body, i);
        n00b_md_node_t val = n00b_tree_node_value(child);
        if (val.node_type == N00B_MD_BLOCK_UL) {
            found_ul = true;
            ASSERT_EQ(n00b_tree_num_children(child), 3);
        }
    }
    ASSERT(found_ul);
}

// ===================================================================
// Multiple paragraphs
// ===================================================================

TEST(test_multiple_paragraphs)
{
    n00b_string_t s = n00b_string_from_cstr("First para\n\nSecond para");

    auto root = n00b_parse_markdown(s);
    ASSERT(root != nullptr);

    auto body = get_body(root);

    int p_count = 0;
    for (size_t i = 0; i < n00b_tree_num_children(body); i++) {
        auto child = n00b_tree_child(body, i);
        n00b_md_node_t val = n00b_tree_node_value(child);
        if (val.node_type == N00B_MD_BLOCK_P) {
            p_count++;
        }
    }
    ASSERT_EQ(p_count, 2);
}

// ===================================================================
// Inline code
// ===================================================================

TEST(test_inline_code)
{
    n00b_string_t s = n00b_string_from_cstr("Use `foo()` here");

    auto root = n00b_parse_markdown(s);
    ASSERT(root != nullptr);

    ASSERT(find_kind(root, N00B_MD_SPAN_CODE));
}

static void run_tests(void)
{
    RUN_TEST(test_empty);
    RUN_TEST(test_paragraph);
    RUN_TEST(test_heading);
    RUN_TEST(test_bold);
    RUN_TEST(test_italic);
    RUN_TEST(test_code_block);
    RUN_TEST(test_unordered_list);
    RUN_TEST(test_multiple_paragraphs);
    RUN_TEST(test_inline_code);
}

TEST_MAIN()
