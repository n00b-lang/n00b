#include "internal.h"

void
writ_emit_init(writ_emit_t *emit)
{
    *emit = (writ_emit_t){.buffer = core_buffer_empty(), .at_line_start = true};
}

static void
emit_vtext(writ_emit_t *emit, const char *format, va_list args)
{
    if (emit->at_line_start && *format && *format != '\n') {
        for (uint32_t i = 0; i < emit->depth; i++) {
            core_buffer_puts(emit->buffer, "    ");
        }
    }
    core_buffer_vprintf(emit->buffer, format, args);
    emit->at_line_start = emit->buffer->byte_len && emit->buffer->data[emit->buffer->byte_len - 1] == '\n';
}

void
writ_emit_text(writ_emit_t *emit, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    emit_vtext(emit, format, args);
    va_end(args);
}

void
writ_emit_line(writ_emit_t *emit, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    emit_vtext(emit, format, args);
    va_end(args);
    core_buffer_putc(emit->buffer, '\n');
    emit->at_line_start = true;
}

void
writ_emit_open(writ_emit_t *emit, const char *format, ...)
{
    if (!*format) {
        writ_emit_line(emit, emit->at_line_start ? "{" : " {");
        emit->depth++;
        return;
    }
    va_list args;
    va_start(args, format);
    emit_vtext(emit, format, args);
    va_end(args);
    core_buffer_puts(emit->buffer, " {\n");
    emit->at_line_start = true;
    emit->depth++;
}

void
writ_emit_close(writ_emit_t *emit, const char *text)
{
    emit->depth--;
    writ_emit_line(emit, "}%s", text ? text : "");
}

void
writ_emit_indent(writ_emit_t *emit, int delta)
{
    emit->depth = (uint32_t)((int)emit->depth + delta);
}

writ_str_t
writ_emit_finish(writ_emit_t *emit, writ_session_t *session)
{
    writ_str_t text = writ_str_from_buffer(session, emit->buffer);
    emit->buffer = nullptr;
    return text;
}

void
writ_emit_discard(writ_emit_t *emit)
{
    core_buffer_free(emit->buffer);
    emit->buffer = nullptr;
}
