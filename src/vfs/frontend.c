#include "vfs/frontend.h"
#include "core/alloc.h"

// ============================================================================
// Platform frontend forward declarations
// ============================================================================

#if defined(__linux__)
extern n00b_result_t(n00b_vfs_frontend_t *)
n00b_vfs_frontend_nfs_new(n00b_vfs_t *vfs, n00b_string_t *mount_point);
#endif

#if defined(N00B_HAVE_FUSE)
extern n00b_result_t(n00b_vfs_frontend_t *)
n00b_vfs_frontend_fuse_new(n00b_vfs_t *vfs, n00b_string_t *mount_point);
#endif

// ============================================================================
// Dispatcher
// ============================================================================

n00b_result_t(n00b_vfs_frontend_t *)
n00b_vfs_frontend_auto(n00b_vfs_t *vfs, n00b_string_t *mount_point)
{
    if (vfs == nullptr || mount_point == nullptr) {
        return n00b_result_err(n00b_vfs_frontend_t *, N00B_VFS_ERR_NULL_ARG);
    }

#if defined(__linux__)
    return n00b_vfs_frontend_nfs_new(vfs, mount_point);
#elif defined(N00B_HAVE_FUSE)
    return n00b_vfs_frontend_fuse_new(vfs, mount_point);
#else
    return n00b_result_err(n00b_vfs_frontend_t *, N00B_VFS_ERR_NOT_SUPPORTED);
#endif
}

// ============================================================================
// Lifecycle wrappers
// ============================================================================

n00b_result_t(bool)
n00b_vfs_frontend_start(n00b_vfs_frontend_t *fe)
{
    if (fe == nullptr || fe->ops == nullptr || fe->ops->start == nullptr) {
        return n00b_result_err(bool, N00B_VFS_ERR_NULL_ARG);
    }

    return fe->ops->start(fe);
}

void
n00b_vfs_frontend_stop(n00b_vfs_frontend_t *fe)
{
    if (fe == nullptr || fe->ops == nullptr || fe->ops->stop == nullptr) {
        return;
    }

    fe->ops->stop(fe);
}

bool
n00b_vfs_frontend_is_running(n00b_vfs_frontend_t *fe)
{
    if (fe == nullptr) {
        return false;
    }

    if (fe->ops != nullptr && fe->ops->is_running != nullptr) {
        return fe->ops->is_running(fe);
    }

    return atomic_load(&fe->running);
}
