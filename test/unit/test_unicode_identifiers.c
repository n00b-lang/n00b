#include "test_unicode_helpers.h"
#include "text/unicode/identifiers.h"
#include "text/unicode/encoding.h"

TEST(test_id_start)
{
    ASSERT(n00b_unicode_is_id_start('A'));
    ASSERT(!n00b_unicode_is_id_start('_'));  // _ is ID_Continue only, not ID_Start
    ASSERT(!n00b_unicode_is_id_start('0'));
    ASSERT(!n00b_unicode_is_id_start(' '));
}

TEST(test_id_continue)
{
    ASSERT(n00b_unicode_is_id_continue('A'));
    ASSERT(n00b_unicode_is_id_continue('0'));
    ASSERT(n00b_unicode_is_id_continue('_'));
    ASSERT(!n00b_unicode_is_id_continue(' '));
}

TEST(test_xid_start)
{
    ASSERT(n00b_unicode_is_xid_start('A'));
    ASSERT(n00b_unicode_is_xid_start(0x00E9)); // é
}

TEST(test_valid_identifier)
{
    ASSERT(n00b_unicode_is_valid_identifier(r"hello"));
    ASSERT(n00b_unicode_is_valid_identifier(r"hello_world"));
    ASSERT(!n00b_unicode_is_valid_identifier(r"123"));
    ASSERT(!n00b_unicode_is_valid_identifier(r""));
}

TEST(test_pattern_syntax)
{
    ASSERT(n00b_unicode_is_pattern_syntax('{'));
    ASSERT(n00b_unicode_is_pattern_syntax('!'));
    ASSERT(!n00b_unicode_is_pattern_syntax('A'));
}

TEST(test_identifier_allowed)
{
    ASSERT(n00b_unicode_is_identifier_allowed('A'));
    ASSERT(n00b_unicode_is_identifier_allowed('0'));
}

static void run_tests(void)
{
    RUN_TEST(test_id_start);
    RUN_TEST(test_id_continue);
    RUN_TEST(test_xid_start);
    RUN_TEST(test_valid_identifier);
    RUN_TEST(test_pattern_syntax);
    RUN_TEST(test_identifier_allowed);
}

TEST_MAIN()
