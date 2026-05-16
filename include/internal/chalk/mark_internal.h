#pragma once

/** @file mark_internal.h — Internal representation of a chalk mark. */

#include <n00b.h>
#include "parsers/json.h"
#include "adt/dict.h"
#include <chalk/n00b_chalk_codec.h>
#include <chalk/n00b_chalk_mark.h>

struct n00b_chalk_mark {
    n00b_dict_t(n00b_string_t *, n00b_json_node_t *) *dict;
    n00b_json_node_t *attestation;
    bool         finalized;
};
