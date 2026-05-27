/* Phase 3 fixture: contains NULL in expression position; engine must emit a violation. */
int
main(void)
{
    int *p = NULL;
    return p == NULL ? 0 : 1;
}
