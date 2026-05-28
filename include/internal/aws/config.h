/**
 * @file internal/aws/config.h
 * @brief Internal layout of `n00b_aws_config_t`.
 *
 * Private to libn00b_aws — consumers go through the opaque handle
 * declared in <aws/n00b_aws.h>. Lives in internal/ so every libn00b_aws
 * translation unit shares the same definition without re-declaring it
 * (which would otherwise be an ODR violation).
 */
#pragma once

#include "n00b.h"
#include "n00b_aws_shim_generated.h"

struct n00b_aws_config_t {
    n00b_aws_shim_config_t *shim;
};
