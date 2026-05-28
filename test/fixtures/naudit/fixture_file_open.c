/* Phase WP-012-prelude fixture: contains n00b_file_open calls
 * that the path-canonicalization rule should fire on. The audit
 * tool flags every call; the reviewer baselines confirmed-correct
 * sites and writes per-site exemptions for the rest.
 *
 * This file contains two intentional calls so the test can assert
 * an exact match count.
 */

int
opens_file_a(const char *path)
{
    auto fr = n00b_file_open(path, .kind = N00B_FILE_KIND_MMAP);
    return fr;
}

int
opens_file_b(const char *path)
{
    auto fr = n00b_file_open(path, .kind = N00B_FILE_KIND_MMAP);
    return fr;
}
