/* WP-002 fixture: uses n00b_alloc family; engine must emit zero violations. */
int
main(void)
{
    int *p = n00b_alloc(sizeof(int));
    n00b_free(p);
    return 0;
}
