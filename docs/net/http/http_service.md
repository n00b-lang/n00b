# `n00b_http_service` - Local Service Routes

`n00b_http_service` is for local tools, test harnesses, and small service
slices that need an HTTP/1 route table without production ingress machinery.

The main API is the documented route helper surface:

```c
auto rr = n00b_http_get(
    svc,
    (n00b_http_endpoint_t){
        .path    = r"/v1/items",
        .handler = get_items_handler,
        .user_data = state,
        .id      = r"getItems",
        .summary = r"Read items",
        .tags    = n00b_http_tags(r"items"),
        .query   = n00b_http_params(
            n00b_http_query_param(r"repo",
                                  .schema_json = r"{\"type\":\"string\"}",
                                  .description = r"Repository URL")),
        .responses = n00b_http_responses(
            n00b_http_json_response(200,
                                    .description = r"Items",
                                    .schema_json = r"{\"type\":\"object\"}")),
    });
```

For JSON writes, use `n00b_http_json_body`:

```c
n00b_http_post(
    svc,
    (n00b_http_endpoint_t){
        .path    = r"/v1/items",
        .handler = post_item_handler,
        .user_data = state,
        .id      = r"submitItem",
        .summary = r"Submit item",
        .tags    = n00b_http_tags(r"items"),
        .body    = n00b_http_json_body(item_schema),
        .responses = n00b_http_responses(
            n00b_http_json_response(202,
                                    .description = r"Accepted",
                                    .schema_json = accepted_schema),
            n00b_http_json_response(422,
                                    .description = r"Invalid request",
                                    .schema_json = error_schema)),
    });
```

Mount discovery once all application routes are registered:

```c
n00b_http_discover(
    svc,
    (n00b_http_discovery_doc_t){
        .service_id = r"item-service",
        .name = r"Item Service",
        .version = r"0.1.0",
        .api_version = r"v1",
        .schemas = n00b_http_schemas(r"/schemas/item-v0.json"),
        .capabilities = n00b_http_capabilities(
            r"items.submit",
            r"items.read"),
    });
```

Discovery registers `GET /openapi.json`, `GET /.well-known/openapi.json`, and
`GET /.well-known/<service-id>`. The lower-level `n00b_http_service_route_spec`
and `n00b_http_service_enable_discovery` APIs remain available, but service
code should prefer the helper surface so route registration and OpenAPI
metadata stay together.

Routes registered with the older `n00b_http_service_route` API still appear in
OpenAPI with a minimal default operation. The documented helpers only add richer
metadata; they are not required just to keep a route discoverable.
