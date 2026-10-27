#pragma once

#include "scanner/token_stream.h"

void scan_stream_reserve_tokens(parse_token_stream_t *ts, size_t needed);
