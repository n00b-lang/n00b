#include "vfs/hooks.h"

n00b_err_t
_n00b_vfs_hooks_run(n00b_vfs_hook_t **hooks, uint32_t nhooks,
                    n00b_vfs_hook_ctx_t *ctx)
{
    if (hooks == nullptr || nhooks == 0) {
        return N00B_VFS_ERR_NONE;
    }

    for (uint32_t i = 0; i < nhooks; i++) {
        n00b_vfs_hook_t *h = hooks[i];
        if (h == nullptr) {
            continue;
        }

        h->fn(ctx, h->cookie);

        if (ctx->denied) {
            if (ctx->deny_err == 0) {
                ctx->deny_err = N00B_VFS_ERR_HOOK_DENIED;
            }
            return ctx->deny_err;
        }
    }

    return N00B_VFS_ERR_NONE;
}
