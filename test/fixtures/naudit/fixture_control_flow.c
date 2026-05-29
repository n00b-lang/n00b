/*
 * WP-019 regression-guard fixture — control flow + attributes.
 *
 * These constructs parsed correctly BEFORE the WP-019 tokenizer fix
 * (they contain no string/char literals). They are kept as a guard:
 * the fix must not break the previously-passing set. Covers
 * if/else, while, do...while, for, for(;;), switch/case/default,
 * empty statement, goto+label, and `__attribute__` in both the
 * declaration and trailing positions.
 *
 * Target input the audit engine PARSES, not n00b-audit source.
 */
int x __attribute__((unused));

void f(void) __attribute__((noreturn));

int
g(int a)
{
    if (a) {
        a = 1;
    }
    else {
        a = 2;
    }

    while (a) {
        a--;
    }

    do {
        a++;
    } while (a < 3);

    for (int i = 0; i < 10; i++) {
        a += i;
    }

    for (;;) {
        break;
    }

    switch (a) {
    case 1:
        break;
    default:
        break;
    }

    ;

    goto done;
done:
    return a;
}
