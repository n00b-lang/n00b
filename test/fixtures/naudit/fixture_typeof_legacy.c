/* WP-003 fixture: legacy __typeof__; engine must flag. */
int
main(void)
{
    int x = 0;
    __typeof__(x) tmp = x;
    return tmp;
}
