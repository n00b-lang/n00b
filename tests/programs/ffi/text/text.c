#include <stddef.h>

size_t
text_length(const char *text)
{
    size_t length = 0;
    while (text[length]) length++;
    return length;
}

const char *
text_greeting(void)
{
    return "h\xc3\xa9llo from C";
}

const char *
text_invalid(void)
{
    return "bad \xff text";
}
