#include <stddef.h>
#include <stdint.h>

typedef struct stmt_state {
    const char *text;
    int length;
    int open;
} stmt_state;

static stmt_state statements[4];

stmt_state *
stmt_open(void)
{
    for (size_t i = 0; i < sizeof statements / sizeof statements[0]; i++) {
        if (!statements[i].open) {
            statements[i] = (stmt_state){.open = 1};
            return &statements[i];
        }
    }
    return NULL;
}

int
bind_text(stmt_state *stmt, const char *text, int length, void (*dtor)(void *))
{
    (void)dtor;
    stmt->text = text;
    stmt->length = length;
    return 0;
}

int
stmt_byte(stmt_state *stmt, int index)
{
    return index < stmt->length ? (unsigned char)stmt->text[index] : -1;
}

void
stmt_release(stmt_state *stmt)
{
    stmt->open = 0;
    stmt->text = NULL;
}
