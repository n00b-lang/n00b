// Broken r-string: unclosed tag bracket syntax.
#include <stdio.h>

typedef struct n00b_string_t {
    unsigned long u8_bytes;
    char         *data;
    unsigned long codepoints;
    void         *styling;
} n00b_string_t;

void foo(void)
{
    // Bug: opening [| without closing |]
    n00b_string_t *s = r"Hello [|b world";
    printf("%s\n", s->data);
}

int main(void) { foo(); return 0; }
