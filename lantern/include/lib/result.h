#pragma once

#include <assert.h>
#include <stdbool.h>

#define core_result_decl(Name, Value, Error)                                      \
    typedef struct Name {                                                        \
        bool is_ok;                                                              \
        union {                                                                  \
            Value value;                                                         \
            Error error;                                                         \
        } result;                                                                \
    } Name

#define core_result_ok(Name, val)                                                \
    ((Name){.is_ok = true, .result.value = (val)})
#define core_result_err(Name, err)                                               \
    ((Name){.is_ok = false, .result.error = (err)})
#define core_result_get_ok(result_value)                                         \
    ({                                                                           \
        auto _core_result = (result_value);                                      \
        assert(_core_result.is_ok);                                              \
        _core_result.result.value;                                               \
    })
#define core_result_get_err(result_value)                                        \
    ({                                                                           \
        auto _core_result = (result_value);                                      \
        assert(!_core_result.is_ok);                                             \
        _core_result.result.error;                                               \
    })
