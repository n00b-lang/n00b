#include "vfs/backend.h"

n00b_result_t(bool)
n00b_vfs_backend_init(n00b_vfs_backend_t *be)
{
    if (be == nullptr || be->ops == nullptr) {
        return n00b_result_err(bool, N00B_VFS_ERR_NULL_ARG);
    }

    if (be->ops->init != nullptr) {
        be->ctx = be->ops->init(be);
    }

    return n00b_result_ok(bool, true);
}

void
n00b_vfs_backend_cleanup(n00b_vfs_backend_t *be)
{
    if (be == nullptr || be->ops == nullptr) {
        return;
    }

    if (be->ops->cleanup != nullptr && be->ctx != nullptr) {
        be->ops->cleanup(be->ctx);
        be->ctx = nullptr;
    }
}
