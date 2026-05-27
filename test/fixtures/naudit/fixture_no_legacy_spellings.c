/* WP-004 fixture: uses C23 spellings; engine must emit zero violations. */
static_assert(sizeof(int) >= 4, "int too small");
thread_local int counter;
int
main(void)
{
    return 0;
}
