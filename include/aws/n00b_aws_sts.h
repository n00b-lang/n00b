/**
 * @file aws/n00b_aws_sts.h
 * @brief STS — Security Token Service.
 *
 * Full Smithy-model coverage of aws-sdk-sts. Every operation
 * returns an `n00b_result_t(T *)` where `T` lives in the n00b GC
 * heap; missing / absent fields surface as `n00b_string_empty()` so
 * callers never have to NULL-check inside a returned struct.
 *
 * Coverage:
 *   - GetCallerIdentity
 *   - AssumeRole
 *   - AssumeRoleWithWebIdentity (IRSA)
 *   - AssumeRoleWithSAML
 *   - GetSessionToken
 *   - GetFederationToken
 *   - DecodeAuthorizationMessage
 *   - GetAccessKeyInfo
 */
#pragma once

#include "n00b.h"
#include "core/string.h"
#include "adt/list.h"
#include "adt/result.h"
#include "aws/n00b_aws.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * Shared response types
 * ------------------------------------------------------------------ */

typedef struct {
    n00b_string_t *access_key_id;
    n00b_string_t *secret_access_key;
    n00b_string_t *session_token;
    /** Unix-ms-since-epoch. -1 if absent. */
    int64_t        expiration_ms;
} n00b_aws_credentials_t;

typedef struct {
    n00b_string_t *assumed_role_id;
    n00b_string_t *arn;
} n00b_aws_assumed_role_user_t;

typedef struct {
    n00b_string_t *federated_user_id;
    n00b_string_t *arn;
} n00b_aws_federated_user_t;

/* ------------------------------------------------------------------
 * GetCallerIdentity
 * ------------------------------------------------------------------ */

typedef struct {
    n00b_string_t *account_id;
    n00b_string_t *arn;
    n00b_string_t *user_id;
} n00b_aws_sts_identity_t;

extern n00b_result_t(n00b_aws_sts_identity_t *)
n00b_aws_sts_get_caller_identity(n00b_aws_config_t *cfg);

/* ------------------------------------------------------------------
 * AssumeRole
 * ------------------------------------------------------------------ */

typedef struct {
    n00b_aws_credentials_t       *credentials;
    n00b_aws_assumed_role_user_t *assumed_role_user;
    /** -1 if absent. */
    int32_t                       packed_policy_size;
    n00b_string_t                *source_identity;
} n00b_aws_sts_assume_role_result_t;

/**
 * @brief sts:AssumeRole.
 *
 * @param cfg                Required.
 * @param role_arn           Required.
 * @param role_session_name  Required.
 * @kw    duration_seconds   900-43200, or 0 for SDK default.
 * @kw    external_id        Cross-account hand-shake value.
 * @kw    mfa_serial         MFA device ARN (required iff @kw mfa_token set).
 * @kw    mfa_token          MFA token code.
 * @kw    policy             Inline JSON policy.
 * @kw    source_identity    Caller-visible identity tag for audit logs.
 * @kw    policy_arns        Managed-policy ARNs (n00b_list of n00b_string_t).
 * @kw    session_tags       Session-tag key/value pairs (parallel lists).
 * @kw    session_tag_values Values for the keys above.
 * @kw    transitive_tag_keys Tag keys carried forward when this session
 *                            assumes another role.
 */
extern n00b_result_t(n00b_aws_sts_assume_role_result_t *)
n00b_aws_sts_assume_role(n00b_aws_config_t *cfg,
                         n00b_string_t     *role_arn,
                         n00b_string_t     *role_session_name) _kargs {
    int32_t                    duration_seconds  = 0;
    n00b_string_t             *external_id       = nullptr;
    n00b_string_t             *mfa_serial        = nullptr;
    n00b_string_t             *mfa_token         = nullptr;
    n00b_string_t             *policy            = nullptr;
    n00b_string_t             *source_identity   = nullptr;
    n00b_list_t(n00b_string_t *) *policy_arns         = nullptr;
    n00b_list_t(n00b_string_t *) *session_tags        = nullptr;
    n00b_list_t(n00b_string_t *) *session_tag_values  = nullptr;
    n00b_list_t(n00b_string_t *) *transitive_tag_keys = nullptr;
};

/* ------------------------------------------------------------------
 * AssumeRoleWithWebIdentity — the IRSA mechanism on EKS.
 * ------------------------------------------------------------------ */

typedef struct {
    n00b_aws_credentials_t       *credentials;
    n00b_aws_assumed_role_user_t *assumed_role_user;
    n00b_string_t                *subject_from_web_identity;
    n00b_string_t                *provider;
    n00b_string_t                *audience;
    n00b_string_t                *source_identity;
    int32_t                       packed_policy_size;
} n00b_aws_sts_web_identity_result_t;

extern n00b_result_t(n00b_aws_sts_web_identity_result_t *)
n00b_aws_sts_assume_role_with_web_identity(n00b_aws_config_t *cfg,
                                           n00b_string_t     *role_arn,
                                           n00b_string_t     *role_session_name,
                                           n00b_string_t     *web_identity_token)
    _kargs {
        n00b_string_t             *provider_id      = nullptr;
        n00b_string_t             *policy           = nullptr;
        int32_t                    duration_seconds = 0;
        n00b_list_t(n00b_string_t *) *policy_arns       = nullptr;
};

/* ------------------------------------------------------------------
 * AssumeRoleWithSAML
 * ------------------------------------------------------------------ */

typedef struct {
    n00b_aws_credentials_t       *credentials;
    n00b_aws_assumed_role_user_t *assumed_role_user;
    int32_t                       packed_policy_size;
    n00b_string_t                *subject;
    n00b_string_t                *subject_type;
    n00b_string_t                *issuer;
    n00b_string_t                *audience;
    n00b_string_t                *name_qualifier;
    n00b_string_t                *source_identity;
} n00b_aws_sts_saml_result_t;

extern n00b_result_t(n00b_aws_sts_saml_result_t *)
n00b_aws_sts_assume_role_with_saml(n00b_aws_config_t *cfg,
                                   n00b_string_t     *role_arn,
                                   n00b_string_t     *principal_arn,
                                   n00b_string_t     *saml_assertion)
    _kargs {
        n00b_string_t             *policy           = nullptr;
        int32_t                    duration_seconds = 0;
        n00b_list_t(n00b_string_t *) *policy_arns       = nullptr;
};

/* ------------------------------------------------------------------
 * GetSessionToken
 * ------------------------------------------------------------------ */

extern n00b_result_t(n00b_aws_credentials_t *)
n00b_aws_sts_get_session_token(n00b_aws_config_t *cfg) _kargs {
    int32_t        duration_seconds = 0;
    n00b_string_t *mfa_serial       = nullptr;
    n00b_string_t *mfa_token        = nullptr;
};

/* ------------------------------------------------------------------
 * GetFederationToken
 * ------------------------------------------------------------------ */

typedef struct {
    n00b_aws_credentials_t   *credentials;
    n00b_aws_federated_user_t *federated_user;
    int32_t                   packed_policy_size;
} n00b_aws_sts_federation_result_t;

extern n00b_result_t(n00b_aws_sts_federation_result_t *)
n00b_aws_sts_get_federation_token(n00b_aws_config_t *cfg,
                                  n00b_string_t     *name) _kargs {
    n00b_string_t             *policy           = nullptr;
    int32_t                    duration_seconds = 0;
    n00b_list_t(n00b_string_t *) *policy_arns       = nullptr;
    n00b_list_t(n00b_string_t *) *tag_keys          = nullptr;
    n00b_list_t(n00b_string_t *) *tag_values        = nullptr;
};

/* ------------------------------------------------------------------
 * DecodeAuthorizationMessage
 * ------------------------------------------------------------------ */

extern n00b_result_t(n00b_string_t *)
n00b_aws_sts_decode_authorization_message(n00b_aws_config_t *cfg,
                                          n00b_string_t     *encoded_message);

/* ------------------------------------------------------------------
 * GetAccessKeyInfo
 * ------------------------------------------------------------------ */

extern n00b_result_t(n00b_string_t *)
n00b_aws_sts_get_access_key_info(n00b_aws_config_t *cfg,
                                 n00b_string_t     *access_key_id);

#ifdef __cplusplus
}
#endif
