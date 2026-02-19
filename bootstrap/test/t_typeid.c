// Test: typeid() produces a unique identifier from a type.
#include <stdio.h>

int
main(void)
{
    // typeid expands to an identifier derived from the type.
    // Using it twice with the same args must produce the same name.
    int typeid("test_", int) = 42;
    printf("typeid ok: %d\n", typeid("test_", int));
    return (typeid("test_", int) == 42) ? 0 : 1;
}
