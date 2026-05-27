/* WP-004 fixture: contains _Static_assert + __thread; engine must flag both. */
_Static_assert(sizeof(int) >= 4, "int too small");
__thread int counter;
int
main(void)
{
    return 0;
}
