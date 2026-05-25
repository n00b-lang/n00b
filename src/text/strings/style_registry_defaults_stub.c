/**
 * @file style_registry_defaults_stub.c
 * @brief No-op defaults for the bootstrap libn00b.
 *
 * The bootstrap libn00b (`n00b_bootstrap`) is the version of libn00b
 * that the static-init helper itself links against — by definition,
 * it cannot use `d{...}` static dict literals because the helper that
 * lowers them does not exist yet at that build stage.
 *
 * The real `n00b_str_registry_install_defaults` implementation lives
 * in `style_registry_defaults.c` (a dict-aware translation unit in
 * `n00b_dict_aware_src`).  The full libn00b consumed by applications
 * and tests gets that version; the bootstrap libn00b gets this stub.
 *
 * The static-init helper never renders rich text — it only emits
 * static images of opaque container metadata — so an empty default
 * style/role set is functionally correct.  `n00b_str_style_lookup` /
 * `n00b_str_role_lookup` simply return "no value" for any name when
 * the helper is the one querying.
 */

#include "text/strings/style_registry.h"

void
n00b_str_registry_install_defaults(void)
{
}
