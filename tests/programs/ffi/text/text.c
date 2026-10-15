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

static size_t
fill(unsigned char *dest, size_t capacity, const char *text)
{
    size_t length = text_length(text);
    for (size_t i = 0; i < length && i < capacity; i++) dest[i] = (unsigned char)text[i];
    return length < capacity ? length : capacity;
}

size_t
text_fill(unsigned char *dest, size_t capacity)
{
    return fill(dest, capacity, "filled by C");
}

size_t
text_fill_invalid(unsigned char *dest, size_t capacity)
{
    return fill(dest, capacity, "bad \xff fill");
}
