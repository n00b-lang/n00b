/* WP-003 fixture: C23 typeof; engine must emit zero violations. */
int
main(void)
{
    int x = 0;
    typeof(x) tmp = x;
    return tmp;
}
