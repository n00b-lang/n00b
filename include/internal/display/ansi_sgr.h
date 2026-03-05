#pragma once

#include "n00b.h"
#include "text/strings/text_style.h"

typedef void (*n00b_ansi_emit_fn)(void *ctx, const char *data, size_t len);

extern void n00b_display_ansi_emit_reset(n00b_ansi_emit_fn emit, void *ctx);
extern void n00b_display_ansi_emit_style(const n00b_text_style_t *style,
                                          n00b_ansi_emit_fn         emit,
                                          void                     *ctx);
