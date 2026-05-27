/* WP-002 fixture: contains malloc + free; engine must flag both. */
#include <stdlib.h>
int
main(void)
{
    int *p = malloc(sizeof(int));
    free(p);
    return 0;
}
