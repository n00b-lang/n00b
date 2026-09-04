# Sources for the n00b hostmeta module: host / build-environment
# metadata collection. This is the C port of chalk's metadata
# "plugins" — cloud instance metadata (AWS IMDSv2, Azure IMDS, GCP),
# the AWS ECS and Lambda execution environments, and nine CI/CD
# systems.
#
# Depends on the HTTP client (src/net/http) for the metadata
# endpoints and on the JSON parser (src/parsers) for their payloads.

n00b_hostmeta_src = files(
    'module.c',
    'ctx.c',
    'util.c',
    'fetch.c',
    'arn.c',
    'ci_common.c',
    'ci_env.c',
    'ci_github.c',
    'ci_teamcity.c',
    'aws_ecs.c',
    'aws_lambda.c',
    'cloud_metadata.c',
)
