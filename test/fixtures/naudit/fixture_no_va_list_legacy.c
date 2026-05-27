/* WP-003 fixture: simple non-variadic function; engine must emit zero violations. */
int
log_msg(const char *fmt)
{
    (void)fmt;
    return 0;
}
