#include "internal.h"

uint32_t
writ_source_add(writ_session_t *session, writ_source_kind_t kind, writ_origin_t origin,
                writ_str_t path, writ_str_t text)
{
    writ_source_t source = {
        .kind        = kind,
        .origin      = origin,
        .path        = writ_str_copy(session, path.data, path.length),
        .text        = writ_str_copy(session, text.data, text.length),
        .line_starts = core_list_new(uint32_t),
    };
    core_list_push(source.line_starts, 0u);
    for (uint32_t i = 0; i < text.length; i++) {
        if (text.data[i] == '\n' || (text.data[i] == '\r' && (i + 1 == text.length || text.data[i + 1] != '\n'))) {
            core_list_push(source.line_starts, i + 1);
        }
    }
    if (!source.origin.first_line) {
        source.origin.first_line = 1;
    }
    if (!source.origin.first_column) {
        source.origin.first_column = 1;
    }
    core_list_push(session->sources, source);
    return (uint32_t)session->sources.len - 1;
}

const writ_source_t *
writ_source_at(const writ_session_t *session, uint32_t id)
{
    return id < session->sources.len ? &session->sources.data[id] : nullptr;
}

writ_span_t
writ_loc_span(const writ_session_t *session, writ_loc_t loc)
{
    const writ_source_t *source = writ_source_at(session, loc.source);
    if (!source) {
        return (writ_span_t){0};
    }
    size_t low = 0;
    size_t high = source->line_starts.len;
    while (high - low > 1) {
        size_t mid = (low + high) / 2;
        if (source->line_starts.data[mid] <= loc.start) {
            low = mid;
        } else {
            high = mid;
        }
    }
    uint32_t column = loc.start - source->line_starts.data[low] + 1;
    if (low == 0) {
        column += source->origin.first_column - 1;
    }
    return (writ_span_t){
        .source_id  = loc.source,
        .start_byte = source->origin.first_byte + loc.start,
        .end_byte   = source->origin.first_byte + loc.end,
        .line       = source->origin.first_line + (uint32_t)low,
        .column     = column,
    };
}

writ_str_t
writ_loc_text(const writ_session_t *session, writ_loc_t loc)
{
    const writ_source_t *source = writ_source_at(session, loc.source);
    if (!source || loc.end < loc.start || loc.end > source->text.length) {
        return (writ_str_t){"", 0};
    }
    return (writ_str_t){source->text.data + loc.start, loc.end - loc.start};
}

writ_loc_t
writ_loc_join(writ_loc_t first, writ_loc_t last)
{
    return (writ_loc_t){first.source, first.start, last.end > first.end ? last.end : first.end};
}
