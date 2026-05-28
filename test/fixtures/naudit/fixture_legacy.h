/* WP-009 Phase 1 fixture: header file containing NULL in expression
 * position. Used to verify that .h extensions dispatch through the
 * built-in language registry to the same C grammar as .c files. */
static int *a_null_pointer = NULL;
