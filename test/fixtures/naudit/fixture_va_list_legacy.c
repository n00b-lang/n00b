/* WP-003 fixture: legacy va_list usage; engine must flag. */
int
log_msg(const char *fmt)
{
    va_list ap;
    va_start(ap, fmt);
    va_end(ap);
    return 0;
}
