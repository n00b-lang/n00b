#include "vfs/types.h"

const char *
n00b_vfs_err_name(n00b_err_t err)
{
    switch (err) {
    case N00B_VFS_ERR_NONE:           return "NONE";
    case N00B_VFS_ERR_NULL_ARG:       return "NULL_ARG";
    case N00B_VFS_ERR_ALLOC:          return "ALLOC";
    case N00B_VFS_ERR_NOT_FOUND:      return "NOT_FOUND";
    case N00B_VFS_ERR_EXISTS:         return "EXISTS";
    case N00B_VFS_ERR_IS_DIR:         return "IS_DIR";
    case N00B_VFS_ERR_NOT_DIR:        return "NOT_DIR";
    case N00B_VFS_ERR_NOT_EMPTY:      return "NOT_EMPTY";
    case N00B_VFS_ERR_PERMISSION:     return "PERMISSION";
    case N00B_VFS_ERR_IO:             return "IO";
    case N00B_VFS_ERR_NO_SPACE:       return "NO_SPACE";
    case N00B_VFS_ERR_INVALID_PATH:   return "INVALID_PATH";
    case N00B_VFS_ERR_INVALID_HANDLE: return "INVALID_HANDLE";
    case N00B_VFS_ERR_CLOSED:         return "CLOSED";
    case N00B_VFS_ERR_BACKEND:        return "BACKEND";
    case N00B_VFS_ERR_CACHE:          return "CACHE";
    case N00B_VFS_ERR_MOUNT:          return "MOUNT";
    case N00B_VFS_ERR_HOOK_DENIED:    return "HOOK_DENIED";
    case N00B_VFS_ERR_STALE:          return "STALE";
    case N00B_VFS_ERR_CROSS_DEVICE:   return "CROSS_DEVICE";
    case N00B_VFS_ERR_NOT_SUPPORTED:  return "NOT_SUPPORTED";
    case N00B_VFS_ERR_READ_ONLY:      return "READ_ONLY";
    default:                          return "UNKNOWN";
    }
}
