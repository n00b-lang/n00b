#include "vks/types.h"

n00b_string_t *
n00b_vks_err_str(n00b_err_t err)
{
    switch (err) {
    case N00B_VKS_ERR_NONE:          return r"NONE";
    case N00B_VKS_ERR_NULL_ARG:      return r"NULL_ARG";
    case N00B_VKS_ERR_ALLOC:         return r"ALLOC";
    case N00B_VKS_ERR_NOT_FOUND:     return r"NOT_FOUND";
    case N00B_VKS_ERR_EXISTS:        return r"EXISTS";
    case N00B_VKS_ERR_BACKEND:       return r"BACKEND";
    case N00B_VKS_ERR_NOT_SUPPORTED: return r"NOT_SUPPORTED";
    case N00B_VKS_ERR_IO:            return r"IO";
    case N00B_VKS_ERR_CLOSED:        return r"CLOSED";
    default:                         return r"UNKNOWN";
    }
}
