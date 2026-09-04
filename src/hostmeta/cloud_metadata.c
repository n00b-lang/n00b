/** @file src/hostmeta/cloud_metadata.c — instance metadata for AWS,
 *        Azure, and GCP.
 *
 *  All three clouds answer on the same link-local address with three
 *  incompatible protocols, so the collector first decides *which*
 *  cloud it is on from local hardware evidence, then speaks only that
 *  cloud's dialect. Probing all three blind would mean two guaranteed
 *  timeouts on every host and a stall on hosts that are on no cloud at
 *  all.
 *
 *  The evidence is DMI / hypervisor identifiers written by the
 *  platform before anything of ours runs:
 *    AWS    https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/identify_ec2_instances.html
 *    AWS    https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/instancedata-data-categories.html
 *    Azure  https://learn.microsoft.com/en-us/azure/virtual-machines/instance-metadata-service
 *    GCP    https://cloud.google.com/compute/docs/metadata/overview
 */

#define N00B_USE_INTERNAL_API

#include "hostmeta/n00b_hostmeta_builtins.h"

#include "internal/hostmeta/hostmeta_internal.h"
#include "internal/net/http/http_h1.h"
#include "text/strings/format.h"
#include "text/strings/string_ops.h"
#include "util/ascii_ci.h"

#define AWS_BASE_URI "/latest/"
#define AWS_MD_URI   "/latest/meta-data/"
#define AWS_DYN_URI  "/latest/dynamic/"

/**
 * Cloud Run exposes no DMI vendor string, so it is identified from the
 * pair of variables the Knative runtime always sets. The second is
 * undocumented but present across every Google functions-framework
 * implementation, e.g.
 * https://github.com/GoogleCloudPlatform/functions-framework-python/blob/02472e7/src/functions_framework/_http/gunicorn.py#L35
 */
#define K_SERVICE                 "K_SERVICE"
#define CLOUD_RUN_TIMEOUT_SECONDS "CLOUD_RUN_TIMEOUT_SECONDS"

/** The IMDS credentials document, whose secrets are redacted on read. */
#define AWS_EC2_SECURITY_CREDS_KEY \
    "_AWS_IDENTITY_CREDENTIALS_EC2_SECURITY_CREDENTIALS_EC2_INSTANCE"

typedef enum : uint8_t {
    HOST_UNKNOWN = 0,
    HOST_AWS,
    HOST_AZURE,
    HOST_GCP,
} host_kind_t;

// ======================================================================
// Shared helpers
// ======================================================================

/** Whether any key in a NUL-terminated name list is subscribed. */
static bool
any_subscribed(n00b_hostmeta_ctx_t *ctx, const char *const *keys)
{
    for (size_t i = 0; keys[i] != nullptr; i++) {
        if (n00b_hostmeta_subscribed(ctx, n00b_hostmeta_str(keys[i]))) {
            return true;
        }
    }
    return false;
}

/** The segment after the last `/`, for GCP's fully-qualified values. */
static n00b_string_t *
last_path_segment(n00b_string_t *s)
{
    if (s == nullptr) {
        return nullptr;
    }
    size_t start = 0;
    for (size_t i = 0; i < s->u8_bytes; i++) {
        if (s->data[i] == '/') {
            start = i + 1;
        }
    }
    if (start == 0) {
        return s;
    }
    return n00b_string_from_raw(s->data + start,
                                (int64_t)(s->u8_bytes - start));
}

/** Copy a subtree into the result under @p key, if present. */
static void
put_subtree(n00b_hostmeta_ctx_t *ctx,
            const char          *key,
            n00b_json_node_t    *root,
            const char          *path)
{
    n00b_json_node_t *node = n00b_hostmeta_json_path(root, path);
    if (node != nullptr) {
        n00b_hostmeta_put(ctx,
                          n00b_hostmeta_str(key),
                          n00b_hostmeta_json_copy(node));
    }
}

/** Copy a string-valued subtree into the result under @p key. */
static void
put_subtree_string(n00b_hostmeta_ctx_t *ctx,
                   const char          *key,
                   n00b_json_node_t    *root,
                   const char          *path)
{
    n00b_json_node_t *node = n00b_hostmeta_json_path(root, path);
    if (n00b_json_is_string(node)) {
        n00b_hostmeta_put_string(ctx,
                                 n00b_hostmeta_str(key),
                                 n00b_json_as_string(node));
    }
}

// ======================================================================
// Host discrimination
// ======================================================================

static bool
is_aws_ec2_host(n00b_hostmeta_ctx_t *ctx, n00b_string_t *vendor)
{
    // Older Xen instances stamp the hypervisor uuid.
    n00b_string_t *uuid = n00b_hostmeta_read_file(ctx->sys_hypervisor_path);
    if (n00b_hostmeta_istarts_with(uuid, "ec2")) {
        return true;
    }

    // Nitro instances identify through the DMI vendor.
    if (n00b_hostmeta_icontains(vendor, "amazon")) {
        return true;
    }

    // Readable only as root on most distributions; equivalent to
    // `dmidecode --string system-uuid`.
    n00b_string_t *product = n00b_hostmeta_read_file(ctx->sys_product_path);
    return n00b_hostmeta_istarts_with(product, "ec2");
}

static bool
is_gcp_host(n00b_hostmeta_ctx_t *ctx, n00b_string_t *vendor)
{
    if (n00b_hostmeta_icontains(vendor, "google")) {
        return true;
    }

    // Cloud Run has no DMI vendor. The Knative variables alone would be
    // spoofable from any shell, so they are corroborated against
    // resolv.conf pointing into Google's internal zone.
    if (n00b_hostmeta_env(K_SERVICE) == nullptr
        || n00b_hostmeta_env(CLOUD_RUN_TIMEOUT_SECONDS) == nullptr) {
        return false;
    }

    n00b_string_t *resolv = n00b_hostmeta_read_file(ctx->resolv_path);
    if (resolv == nullptr) {
        return false;
    }

    // Per resolv.conf(5) a comment is `;` or `#` in column one, and a
    // directive's keyword starts the line — so a first-column comment
    // marker is the only thing that can neutralize a line.
    n00b_list_t(n00b_string_t *) *lines = n00b_hostmeta_split_lines(resolv);
    for (size_t i = 0; i < n00b_list_len(*lines); i++) {
        n00b_string_t *line = n00b_list_get(*lines, i);
        if (line->u8_bytes == 0 || line->data[0] == ';' || line->data[0] == '#') {
            continue;
        }
        if (n00b_hostmeta_icontains(line, "google.internal")) {
            return true;
        }
    }
    return false;
}

static host_kind_t
detect_host(n00b_hostmeta_ctx_t *ctx, n00b_string_t *vendor)
{
    if (is_aws_ec2_host(ctx, vendor)) {
        return HOST_AWS;
    }
    if (n00b_hostmeta_icontains(vendor, "microsoft")) {
        return HOST_AZURE;
    }
    if (is_gcp_host(ctx, vendor)) {
        return HOST_GCP;
    }
    return HOST_UNKNOWN;
}

// ======================================================================
// AWS IMDSv2
// ======================================================================

static n00b_http_h1_headers_t *
imds_headers(n00b_string_t *token)
{
    n00b_http_h1_headers_t *h = n00b_http_h1_headers_new();
    n00b_http_h1_headers_set(h, "X-aws-ec2-metadata-token", token->data);
    return h;
}

/**
 * Obtain an IMDSv2 session token.
 *
 * @return The token, or nullptr with a failure recorded explaining
 *         which of the three usual causes applies.
 */
static n00b_string_t *
aws_get_token(n00b_hostmeta_ctx_t *ctx)
{
    n00b_string_t *url = n00b_hostmeta_metadata_url(ctx,
                                                    AWS_BASE_URI "api/token");

    n00b_http_h1_headers_t *h = n00b_http_h1_headers_new();
    n00b_http_h1_headers_set(h, "X-aws-ec2-metadata-token-ttl-seconds", "10");

    n00b_hostmeta_fetch_t f = n00b_hostmeta_fetch(ctx,
                                                  url,
                                                  .method  = "PUT",
                                                  .headers = h,
                                                  .strict  = true);
    if (f.ok && f.body != nullptr && f.body->u8_bytes > 0) {
        return f.body;
    }

    n00b_string_t *key = r"_OP_CLOUD_METADATA";

    if (f.status == 403) {
        n00b_hostmeta_add_failure(
            ctx,
            key,
            "IMDS_DISABLED",
            f.error,
            "IMDSv2 returned 403 Forbidden, which means instance metadata "
            "is disabled. Enable IMDSv2 to collect more information about "
            "this EC2 instance. See https://docs.aws.amazon.com/AWSEC2/"
            "latest/UserGuide/instancedata-data-retrieval.html"
            "#instance-metadata-returns.");
        return nullptr;
    }

    // A container in its own network namespace is two hops from IMDS;
    // the default PUT response hop limit of 1 drops the token on the
    // way back, which presents as a timeout rather than a status.
    n00b_hostmeta_add_failure(
        ctx,
        key,
        "IMDS_UNREACHABLE",
        f.error,
        "Could not obtain an IMDSv2 token. If this is running in a "
        "network-namespaced container, the instance's PUT response hop "
        "limit must be at least 2. See https://docs.aws.amazon.com/AWSEC2/"
        "latest/UserGuide/configuring-instance-metadata-options.html");
    return nullptr;
}

/** GET one IMDS path and store the body verbatim. */
static void
aws_one_item(n00b_hostmeta_ctx_t *ctx,
             n00b_string_t       *token,
             const char          *key,
             const char          *path)
{
    n00b_string_t *k = n00b_hostmeta_str(key);
    if (!n00b_hostmeta_subscribed(ctx, k)) {
        return;
    }

    n00b_hostmeta_fetch_t f = n00b_hostmeta_fetch(
        ctx,
        n00b_hostmeta_metadata_url(ctx, path),
        .headers = imds_headers(token));
    if (f.ok) {
        n00b_hostmeta_put_string(ctx, k, f.body);
    }
}

/** GET one IMDS path whose body is a newline-delimited list. */
static void
aws_list_key(n00b_hostmeta_ctx_t *ctx,
             n00b_string_t       *token,
             const char          *key,
             const char          *path)
{
    n00b_string_t *k = n00b_hostmeta_str(key);
    if (!n00b_hostmeta_subscribed(ctx, k)) {
        return;
    }

    n00b_hostmeta_fetch_t f = n00b_hostmeta_fetch(
        ctx,
        n00b_hostmeta_metadata_url(ctx, path),
        .headers = imds_headers(token));
    if (!f.ok || f.body->u8_bytes == 0) {
        return;
    }

    n00b_list_t(n00b_string_t *) *lines = n00b_hostmeta_split_lines(f.body);
    n00b_json_node_t *arr = n00b_json_array_new();
    for (size_t i = 0; i < n00b_list_len(*lines); i++) {
        n00b_json_array_push(arr,
                             n00b_json_string_new_from_n00b(
                                 n00b_list_get(*lines, i)));
    }
    n00b_hostmeta_put(ctx, k, arr);
}

/** Replace the live secret fields of an IMDS credentials document. */
static void
redact_credentials(n00b_json_node_t *node)
{
    if (!n00b_json_is_object(node)) {
        return;
    }
    if (n00b_json_object_get_cstr(node, "SecretAccessKey") != nullptr) {
        n00b_json_object_put(node,
                             "SecretAccessKey",
                             n00b_json_string_new("<<redacted>>"));
    }
    if (n00b_json_object_get_cstr(node, "Token") != nullptr) {
        n00b_json_object_put(node,
                             "Token",
                             n00b_json_string_new("<<redacted>>"));
    }
}

/** GET one IMDS path whose body is a JSON document. */
static n00b_json_node_t *
aws_fetch_json(n00b_hostmeta_ctx_t *ctx,
               n00b_string_t       *token,
               const char          *path)
{
    n00b_hostmeta_fetch_t f = n00b_hostmeta_fetch(
        ctx,
        n00b_hostmeta_metadata_url(ctx, path),
        .headers = imds_headers(token));
    if (!f.ok) {
        return nullptr;
    }

    n00b_string_t *err = nullptr;
    return n00b_hostmeta_parse_json(f.body, &err);
}

static void
aws_json_key(n00b_hostmeta_ctx_t *ctx,
             n00b_string_t       *token,
             const char          *key,
             const char          *path)
{
    n00b_string_t *k = n00b_hostmeta_str(key);
    if (!n00b_hostmeta_subscribed(ctx, k)) {
        return;
    }

    n00b_json_node_t *node = aws_fetch_json(ctx, token, path);
    if (node == nullptr) {
        return;
    }

    // The credentials document holds live, usable keys; it is reported
    // for its structure and expiry, never for its secrets.
    if (n00b_ascii_ci_eq(key, AWS_EC2_SECURITY_CREDS_KEY)) {
        redact_credentials(node);
    }
    n00b_hostmeta_put(ctx, k, node);
}

/** GET a JSON document and store one string field out of it. */
static void
aws_extract_json_key(n00b_hostmeta_ctx_t *ctx,
                     n00b_string_t       *token,
                     const char          *key,
                     const char          *path,
                     const char          *subkey)
{
    n00b_string_t *k = n00b_hostmeta_str(key);
    if (!n00b_hostmeta_subscribed(ctx, k)) {
        return;
    }

    n00b_json_node_t *node = aws_fetch_json(ctx, token, path);
    if (node == nullptr) {
        return;
    }
    put_subtree_string(ctx, key, node, subkey);
}

/**
 * Collect instance tags.
 *
 * The tags endpoint lists names, then answers one request per name.
 * Tags are opt-in per instance, so an absent endpoint is silence, not
 * a failure; a name that lists but will not fetch is a failure.
 */
static void
aws_get_tags(n00b_hostmeta_ctx_t *ctx,
             n00b_string_t       *token,
             const char          *key,
             const char          *path)
{
    n00b_string_t *k = n00b_hostmeta_str(key);
    if (!n00b_hostmeta_subscribed(ctx, k)) {
        return;
    }

    n00b_hostmeta_fetch_t listing = n00b_hostmeta_fetch(
        ctx,
        n00b_hostmeta_metadata_url(ctx, path),
        .headers = imds_headers(token));
    if (!listing.ok || listing.body->u8_bytes == 0) {
        return;
    }

    n00b_list_t(n00b_string_t *) *names = n00b_hostmeta_split_lines(
        listing.body);
    n00b_json_node_t *tags = n00b_json_object_new();

    for (size_t i = 0; i < n00b_list_len(*names); i++) {
        n00b_string_t *name = n00b_list_get(*names, i);
        n00b_string_t *url  = n00b_cformat(
            "[|#|]/[|#|]",
            n00b_hostmeta_metadata_url(ctx, path),
            name);

        n00b_hostmeta_fetch_t v = n00b_hostmeta_fetch(ctx,
                                                      url,
                                                      .headers = imds_headers(
                                                          token));
        if (!v.ok) {
            n00b_hostmeta_add_failure(ctx,
                                      k,
                                      "CLOUD_TAG_FETCH_ERROR",
                                      v.error,
                                      "Could not retrieve the value of an "
                                      "instance tag that IMDS listed");
            continue;
        }
        if (v.body->u8_bytes > 0) {
            n00b_json_object_put_n00b(tags,
                                      name,
                                      n00b_json_string_new_from_n00b(v.body));
        }
    }

    n00b_hostmeta_put(ctx, k, tags);
}

/**
 * Name the AWS service this process is running under.
 *
 * IMDS itself cannot distinguish these — an ECS task and an EKS pod
 * both sit on an EC2 instance and see the same instance document — so
 * the discrimination is by injected environment.
 */
static void
aws_service_type(n00b_hostmeta_ctx_t *ctx)
{
    n00b_string_t *key = r"_OP_CLOUD_PROVIDER_SERVICE_TYPE";
    if (!n00b_hostmeta_subscribed(ctx, key)) {
        return;
    }

    if (n00b_hostmeta_env("ECS_CONTAINER_METADATA_URI") != nullptr
        || n00b_hostmeta_env("ECS_CONTAINER_METADATA_URI_V4") != nullptr) {
        n00b_hostmeta_put_string(ctx, key, r"aws_ecs");
        return;
    }

    // Fargate-backed EKS pods see the kubernetes service env but no
    // node-level marker. This misreads a self-managed k8s running on a
    // single EC2 instance as EKS; chalk accepts that, and the instance
    // document under `_AWS_*` still tells the true story.
    // https://docs.aws.amazon.com/eks/latest/userguide/fargate.html
    if (n00b_hostmeta_env("KUBERNETES_PORT") != nullptr
        || n00b_hostmeta_env("KUBERNETES_SERVICE_HOST") != nullptr) {
        n00b_hostmeta_put_string(ctx, key, r"aws_eks");
        return;
    }

    n00b_hostmeta_put_string(ctx, key, r"aws_ec2");
}

static void
collect_aws(n00b_hostmeta_ctx_t *ctx)
{
    n00b_hostmeta_put_string(ctx, r"_OP_CLOUD_PROVIDER", r"aws");

    // The service type comes from local evidence, so it is reported
    // even when IMDS is unreachable.
    aws_service_type(ctx);

    // The board asset tag carries the instance id on Nitro instances
    // and needs no token.
    n00b_string_t *asset_tag = n00b_hostmeta_read_file(
        ctx->sys_board_asset_tag_path);
    if (asset_tag != nullptr) {
        n00b_string_t *trimmed = n00b_unicode_str_trim(asset_tag);
        if (n00b_hostmeta_istarts_with(trimmed, "i-")) {
            n00b_hostmeta_put_string(ctx, r"_AWS_INSTANCE_ID", trimmed);
        }
    }

    n00b_string_t *token = aws_get_token(ctx);
    if (token == nullptr) {
        return;
    }

    aws_json_key(ctx,
                 token,
                 "_AWS_INSTANCE_IDENTITY_DOCUMENT",
                 AWS_DYN_URI "instance-identity/document");
    aws_extract_json_key(ctx,
                         token,
                         "_OP_CLOUD_PROVIDER_ACCOUNT_INFO",
                         AWS_DYN_URI "instance-identity/document",
                         "accountId");
    aws_extract_json_key(ctx,
                         token,
                         "_OP_CLOUD_PROVIDER_INSTANCE_TYPE",
                         AWS_DYN_URI "instance-identity/document",
                         "instanceType");
    aws_extract_json_key(ctx,
                         token,
                         "_OP_CLOUD_PROVIDER_INSTANCE_ARCH",
                         AWS_DYN_URI "instance-identity/document",
                         "architecture");

    aws_one_item(ctx,
                 token,
                 "_AWS_INSTANCE_IDENTITY_PKCS7",
                 AWS_DYN_URI "instance-identity/pkcs7");
    aws_one_item(ctx,
                 token,
                 "_AWS_INSTANCE_IDENTITY_SIGNATURE",
                 AWS_DYN_URI "instance-identity/signature");
    aws_one_item(ctx,
                 token,
                 "_AWS_INSTANCE_MONITORING",
                 AWS_DYN_URI "fws/instance-monitoring");

    aws_one_item(ctx, token, "_AWS_AMI_ID", AWS_MD_URI "ami-id");
    aws_one_item(ctx, token, "_AWS_AMI_LAUNCH_INDEX", AWS_MD_URI "ami-launch-index");
    aws_one_item(ctx, token, "_AWS_AMI_MANIFEST_PATH", AWS_MD_URI "ami-manifest-path");
    aws_one_item(ctx, token, "_AWS_ANCESTOR_AMI_IDS", AWS_MD_URI "ancestor-ami-ids");
    aws_one_item(ctx,
                 token,
                 "_AWS_AUTOSCALING_TARGET_LIFECYCLE_STATE",
                 AWS_MD_URI "autoscaling/target-lifecycle-state");
    aws_one_item(ctx, token, "_AWS_AZ", AWS_MD_URI "placement/availability-zone");
    aws_one_item(ctx, token, "_AWS_AZ_ID", AWS_MD_URI "placement/availability-zone-id");
    aws_one_item(ctx,
                 token,
                 "_AWS_BLOCK_DEVICE_MAPPING_AMI",
                 AWS_MD_URI "block-device-mapping/ami");
    aws_one_item(ctx,
                 token,
                 "_AWS_BLOCK_DEVICE_MAPPING_ROOT",
                 AWS_MD_URI "block-device-mapping/root");
    aws_one_item(ctx,
                 token,
                 "_AWS_BLOCK_DEVICE_MAPPING_SWAP",
                 AWS_MD_URI "block-device-mapping/swap");
    aws_one_item(ctx, token, "_AWS_DEDICATED_HOST_ID", AWS_MD_URI "placement/host-id");
    aws_one_item(ctx, token, "_AWS_HOSTNAME", AWS_MD_URI "hostname");
    aws_one_item(ctx, token, "_AWS_INSTANCE_ACTION", AWS_MD_URI "instance-action");
    aws_one_item(ctx, token, "_AWS_INSTANCE_ID", AWS_MD_URI "instance-id");
    aws_one_item(ctx, token, "_AWS_INSTANCE_LIFE_CYCLE", AWS_MD_URI "instance-life-cycle");
    aws_one_item(ctx, token, "_AWS_INSTANCE_TYPE", AWS_MD_URI "instance-type");
    aws_one_item(ctx, token, "_AWS_IPV6_ADDR", AWS_MD_URI "ipv6");
    aws_one_item(ctx, token, "_AWS_KERNEL_ID", AWS_MD_URI "kernel-id");
    aws_one_item(ctx, token, "_AWS_LOCAL_HOSTNAME", AWS_MD_URI "local-hostname");
    aws_one_item(ctx, token, "_AWS_LOCAL_IPV4_ADDR", AWS_MD_URI "local-ipv4");
    aws_one_item(ctx, token, "_AWS_MAC", AWS_MD_URI "mac");
    aws_one_item(ctx, token, "_AWS_METRICS_VHOSTMD", AWS_MD_URI "metrics/vhostmd");
    aws_one_item(ctx, token, "_AWS_OPENSSH_PUBKEY", AWS_MD_URI "public-keys/0/openssh-key");
    aws_one_item(ctx, token, "_AWS_PARTITION_NAME", AWS_MD_URI "services/partition");
    aws_one_item(ctx, token, "_AWS_PARTITION_NUMBER", AWS_MD_URI "placement/partition-number");
    aws_one_item(ctx, token, "_AWS_PLACEMENT_GROUP", AWS_MD_URI "placement/group-name");
    aws_one_item(ctx, token, "_AWS_PRODUCT_CODES", AWS_MD_URI "product-codes");
    aws_one_item(ctx, token, "_AWS_PUBLIC_HOSTNAME", AWS_MD_URI "public-hostname");
    aws_one_item(ctx, token, "_AWS_PUBLIC_IPV4_ADDR", AWS_MD_URI "public-ipv4");
    aws_one_item(ctx, token, "_OP_CLOUD_PROVIDER_IP", AWS_MD_URI "public-ipv4");
    aws_one_item(ctx, token, "_AWS_RAMDISK_ID", AWS_MD_URI "ramdisk-id");
    aws_one_item(ctx, token, "_AWS_REGION", AWS_MD_URI "placement/region");
    aws_one_item(ctx, token, "_OP_CLOUD_PROVIDER_REGION", AWS_MD_URI "placement/region");
    aws_one_item(ctx, token, "_AWS_RESERVATION_ID", AWS_MD_URI "reservation-id");
    aws_one_item(ctx, token, "_AWS_RESOURCE_DOMAIN", AWS_MD_URI "services/domain");
    aws_one_item(ctx, token, "_AWS_SPOT_INSTANCE_ACTION", AWS_MD_URI "spot/instance-action");
    aws_one_item(ctx, token, "_AWS_SPOT_TERMINATION_TIME", AWS_MD_URI "spot/termination-time");

    aws_list_key(ctx, token, "_AWS_SECURITY_GROUPS", AWS_MD_URI "security-groups");

    aws_json_key(ctx,
                 token,
                 "_AWS_EVENTS_MAINTENANCE_HISTORY",
                 AWS_MD_URI "events/maintenance/history");
    aws_json_key(ctx,
                 token,
                 "_AWS_EVENTS_MAINTENANCE_SCHEDULED",
                 AWS_MD_URI "events/maintenance/scheduled");
    aws_json_key(ctx,
                 token,
                 "_AWS_EVENTS_RECOMMENDATIONS_REBALANCE",
                 AWS_MD_URI "events/recommendations/rebalance");
    aws_json_key(ctx, token, "_AWS_IAM_INFO", AWS_MD_URI "iam/info");
    aws_json_key(ctx,
                 token,
                 "_AWS_IDENTITY_CREDENTIALS_EC2_INFO",
                 AWS_MD_URI "identity-credentials/ec2/info");
    aws_json_key(ctx,
                 token,
                 AWS_EC2_SECURITY_CREDS_KEY,
                 AWS_MD_URI "identity-credentials/ec2/security-credentials/ec2-instance");

    aws_get_tags(ctx, token, "_AWS_TAGS", AWS_MD_URI "tags/instance");
    aws_get_tags(ctx, token, "_OP_CLOUD_PROVIDER_TAGS", AWS_MD_URI "tags/instance");

    // Network attributes hang off the primary interface's MAC, so they
    // can only be asked for once the MAC is known.
    n00b_json_node_t *mac_node = n00b_hostmeta_get(ctx, r"_AWS_MAC");
    if (!n00b_json_is_string(mac_node)) {
        return;
    }
    n00b_string_t *mac_base = n00b_cformat(
        AWS_MD_URI "network/interfaces/macs/[|#|]",
        n00b_json_as_string(mac_node));

    aws_one_item(ctx,
                 token,
                 "_AWS_VPC_ID",
                 n00b_unicode_str_cat(mac_base, r"/vpc-id")->data);
    aws_one_item(ctx,
                 token,
                 "_AWS_SUBNET_ID",
                 n00b_unicode_str_cat(mac_base, r"/subnet-id")->data);
    aws_one_item(ctx,
                 token,
                 "_AWS_INTERFACE_ID",
                 n00b_unicode_str_cat(mac_base, r"/interface-id")->data);
    aws_list_key(ctx,
                 token,
                 "_AWS_SECURITY_GROUP_IDS",
                 n00b_unicode_str_cat(mac_base, r"/security-group-ids")->data);
}

// ======================================================================
// Azure IMDS
// ======================================================================

static const char *const k_azure_keys[] = {
    "_AZURE_INSTANCE_METADATA",
    "_OP_CLOUD_PROVIDER_IP",
    "_OP_CLOUD_PROVIDER_REGION",
    "_OP_CLOUD_PROVIDER_TAGS",
    "_OP_CLOUD_PROVIDER_ACCOUNT_INFO",
    "_OP_CLOUD_PROVIDER_INSTANCE_TYPE",
    nullptr,
};

/** First public IPv4 across the instance's interfaces, if any. */
static n00b_string_t *
azure_public_ip(n00b_json_node_t *root)
{
    n00b_json_node_t *ifaces = n00b_hostmeta_json_path(root, "network.interface");
    if (!n00b_json_is_array(ifaces)) {
        return nullptr;
    }

    size_t n_ifaces = n00b_json_array_len(ifaces);
    for (size_t i = 0; i < n_ifaces; i++) {
        n00b_json_node_t *addrs = n00b_hostmeta_json_path(
            n00b_json_array_get(ifaces, i),
            "ipv4.ipAddress");
        if (!n00b_json_is_array(addrs)) {
            continue;
        }
        size_t n_addrs = n00b_json_array_len(addrs);
        for (size_t j = 0; j < n_addrs; j++) {
            n00b_json_node_t *ip = n00b_hostmeta_json_path(
                n00b_json_array_get(addrs, j),
                "publicIpAddress");
            if (n00b_json_is_string(ip)
                && n00b_json_as_string(ip)->u8_bytes > 0) {
                return n00b_json_as_string(ip);
            }
        }
    }
    return nullptr;
}

static void
collect_azure(n00b_hostmeta_ctx_t *ctx)
{
    n00b_hostmeta_put_string(ctx, r"_OP_CLOUD_PROVIDER", r"azure");

    if (!any_subscribed(ctx, k_azure_keys)) {
        return;
    }

    n00b_http_h1_headers_t *h = n00b_http_h1_headers_new();
    n00b_http_h1_headers_set(h, "Metadata", "true");

    n00b_hostmeta_fetch_t f = n00b_hostmeta_fetch(
        ctx,
        n00b_hostmeta_metadata_url(ctx,
                                   "/metadata/instance?api-version=2021-02-01"),
        .headers = h,
        .strict  = true);
    if (!f.ok) {
        n00b_hostmeta_add_failure(
            ctx,
            r"_OP_CLOUD_METADATA",
            "AZURE_IMDS_ERROR",
            f.error,
            "The Azure metadata endpoint at /metadata/instance was "
            "unreachable or returned a non-2xx response");
        return;
    }

    n00b_string_t    *parse_err = nullptr;
    n00b_json_node_t *root      = n00b_hostmeta_parse_json(f.body, &parse_err);
    if (!n00b_json_is_object(root)) {
        n00b_hostmeta_add_failure(
            ctx,
            r"_OP_CLOUD_METADATA",
            "AZURE_IMDS_PARSE_ERROR",
            parse_err,
            "The Azure IMDS endpoint returned data that is not a JSON "
            "object");
        return;
    }

    n00b_hostmeta_put(ctx, r"_AZURE_INSTANCE_METADATA", root);
    n00b_hostmeta_put_string(ctx,
                             r"_OP_CLOUD_PROVIDER_IP",
                             azure_public_ip(root));

    put_subtree(ctx, "_OP_CLOUD_PROVIDER_TAGS", root, "compute.tagsList");
    put_subtree_string(ctx,
                       "_OP_CLOUD_PROVIDER_ACCOUNT_INFO",
                       root,
                       "compute.subscriptionId");
    put_subtree_string(ctx, "_OP_CLOUD_PROVIDER_REGION", root, "compute.location");
    put_subtree_string(ctx,
                       "_OP_CLOUD_PROVIDER_INSTANCE_TYPE",
                       root,
                       "compute.vmSize");
}

// ======================================================================
// GCP metadata server
// ======================================================================

static const char *const k_gcp_keys[] = {
    "_GCP_INSTANCE_METADATA",
    "_GCP_PROJECT_METADATA",
    "_OP_CLOUD_PROVIDER_IP",
    "_OP_CLOUD_PROVIDER_REGION",
    "_OP_CLOUD_PROVIDER_TAGS",
    "_OP_CLOUD_PROVIDER_ACCOUNT_INFO",
    "_OP_CLOUD_PROVIDER_INSTANCE_TYPE",
    nullptr,
};

static n00b_http_h1_headers_t *
gcp_headers(void)
{
    n00b_http_h1_headers_t *h = n00b_http_h1_headers_new();
    n00b_http_h1_headers_set(h, "Metadata-Flavor", "Google");
    return h;
}

/** First external IPv4 across the instance's interfaces, if any. */
static n00b_string_t *
gcp_external_ip(n00b_json_node_t *root)
{
    n00b_json_node_t *ifaces = n00b_hostmeta_json_path(root, "networkInterfaces");
    if (!n00b_json_is_array(ifaces)) {
        return nullptr;
    }

    size_t n_ifaces = n00b_json_array_len(ifaces);
    for (size_t i = 0; i < n_ifaces; i++) {
        n00b_json_node_t *configs = n00b_hostmeta_json_path(
            n00b_json_array_get(ifaces, i),
            "accessConfigs");
        if (!n00b_json_is_array(configs)) {
            continue;
        }
        size_t n_configs = n00b_json_array_len(configs);
        for (size_t j = 0; j < n_configs; j++) {
            n00b_json_node_t *ip = n00b_hostmeta_json_path(
                n00b_json_array_get(configs, j),
                "externalIp");
            if (n00b_json_is_string(ip)
                && n00b_json_as_string(ip)->u8_bytes > 0) {
                return n00b_json_as_string(ip);
            }
        }
    }
    return nullptr;
}

static void
collect_gcp(n00b_hostmeta_ctx_t *ctx)
{
    n00b_hostmeta_put_string(ctx, r"_OP_CLOUD_PROVIDER", r"gcp");

    if (n00b_hostmeta_env(K_SERVICE) != nullptr
        && n00b_hostmeta_env(CLOUD_RUN_TIMEOUT_SECONDS) != nullptr) {
        n00b_hostmeta_put_string(ctx,
                                 r"_OP_CLOUD_PROVIDER_SERVICE_TYPE",
                                 r"gcp_cloud_run_service");
    }

    if (!any_subscribed(ctx, k_gcp_keys)) {
        return;
    }

    n00b_string_t *project_key = r"_GCP_PROJECT_METADATA";
    if (n00b_hostmeta_subscribed(ctx, project_key)) {
        n00b_hostmeta_fetch_t p = n00b_hostmeta_fetch(
            ctx,
            n00b_hostmeta_metadata_url(ctx,
                                       "/computeMetadata/v1/project/"
                                       "?recursive=true"),
            .headers = gcp_headers(),
            .strict  = true);
        if (!p.ok) {
            n00b_hostmeta_add_failure(
                ctx,
                project_key,
                "GCP_PROJECT_METADATA_ERROR",
                p.error,
                "The GCP metadata endpoint at /computeMetadata/v1/project "
                "was unreachable or returned a non-2xx response");
        }
        else {
            n00b_string_t    *err  = nullptr;
            n00b_json_node_t *proj = n00b_hostmeta_parse_json(p.body, &err);
            if (n00b_json_is_object(proj)) {
                n00b_hostmeta_put(ctx, project_key, proj);
            }
        }
    }

    n00b_hostmeta_fetch_t f = n00b_hostmeta_fetch(
        ctx,
        n00b_hostmeta_metadata_url(ctx,
                                   "/computeMetadata/v1/instance/"
                                   "?recursive=true"),
        .headers = gcp_headers(),
        .strict  = true);
    if (!f.ok) {
        n00b_hostmeta_add_failure(
            ctx,
            r"_OP_CLOUD_METADATA",
            "GCP_IMDS_ERROR",
            f.error,
            "The GCP metadata endpoint at /computeMetadata/v1/instance was "
            "unreachable or returned a non-2xx response");
        return;
    }

    n00b_string_t    *parse_err = nullptr;
    n00b_json_node_t *root      = n00b_hostmeta_parse_json(f.body, &parse_err);
    if (!n00b_json_is_object(root)) {
        n00b_hostmeta_add_failure(
            ctx,
            r"_OP_CLOUD_METADATA",
            "GCP_IMDS_PARSE_ERROR",
            parse_err,
            "The GCP IMDS endpoint returned data that is not a JSON object");
        return;
    }

    n00b_hostmeta_put_string(ctx,
                             r"_OP_CLOUD_PROVIDER_IP",
                             gcp_external_ip(root));
    n00b_hostmeta_put(ctx, r"_GCP_INSTANCE_METADATA", root);
    put_subtree(ctx, "_OP_CLOUD_PROVIDER_TAGS", root, "tags");
    put_subtree(ctx, "_OP_CLOUD_PROVIDER_ACCOUNT_INFO", root, "serviceAccounts");

    // Zone and machine type are reported as full resource paths
    // (`projects/N/zones/us-central1-a`); only the leaf is meaningful
    // next to the other providers' values.
    n00b_json_node_t *zone = n00b_hostmeta_json_path(root, "zone");
    if (n00b_json_is_string(zone)) {
        n00b_hostmeta_put_string(ctx,
                                 r"_OP_CLOUD_PROVIDER_REGION",
                                 last_path_segment(n00b_json_as_string(zone)));
    }

    n00b_json_node_t *machine = n00b_hostmeta_json_path(root, "machineType");
    if (n00b_json_is_string(machine)) {
        n00b_hostmeta_put_string(
            ctx,
            r"_OP_CLOUD_PROVIDER_INSTANCE_TYPE",
            last_path_segment(n00b_json_as_string(machine)));
    }
}

// ======================================================================
// Collector
// ======================================================================

static void
cloud_metadata_run_time(n00b_hostmeta_collector_t *self,
                        n00b_hostmeta_ctx_t       *ctx)
{
    (void)self;

    n00b_string_t *vendor = n00b_hostmeta_read_file(ctx->sys_vendor_path);
    if (vendor != nullptr) {
        vendor = n00b_unicode_str_trim(vendor);
    }

    switch (detect_host(ctx, vendor)) {
    case HOST_AWS:
        collect_aws(ctx);
        break;
    case HOST_AZURE:
        collect_azure(ctx);
        break;
    case HOST_GCP:
        collect_gcp(ctx);
        break;
    case HOST_UNKNOWN:
        // Not on a cloud we recognize. The vendor string below is still
        // worth reporting: it is what a future reader needs to work out
        // why nothing else was collected.
        break;
    }

    n00b_hostmeta_put_string(ctx, r"_OP_CLOUD_SYS_VENDOR", vendor);
}

n00b_hostmeta_collector_t n00b_hostmeta_collector_cloud_metadata = {
    .run_time = cloud_metadata_run_time,
};
