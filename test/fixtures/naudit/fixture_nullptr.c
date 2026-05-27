/* Phase 3 fixture: contains nullptr; engine must emit zero violations. */
int
main(void)
{
    int *p = nullptr;
    return p == nullptr ? 0 : 1;
}
