# Net API Ergonomics Audit

This audit applies the n00b API-design rule against opaque positional
argument trains: required values that form one conceptual object should be
grouped in a public descriptor; optional values should usually be `_kargs`.
Low-level wire encoders and byte-copy primitives can keep positional
arguments when the order is conventional and the call site stays readable.

## Simplified In This Pass

- HTTP service docs:
  - `n00b_http_discover(svc, doc)` now carries `.service_id` inside the
    discovery descriptor.
  - `n00b_http_json_response(status, .description = ..., .schema_json = ...)`
    keeps only the status positional.
  - `n00b_http_response_writer_text(resp, body, .content_type = ...)` defaults
    to `text/plain`.
- HTTP/3:
  - `n00b_h3_client_request(client, request_spec)` groups method, authority,
    path, headers, body, and stream-open behavior.
  - `n00b_h3_inbound_request_respond(req, response_spec)` groups status,
    headers, body, and stream-close behavior.
  - Streaming helpers now take `n00b_h3_data_t` or
    `n00b_h3_response_headers_t`.
  - Raw escape hatches remain for callers already holding separate fields.
- QUIC cert reload:
  - `n00b_quic_endpoint_reload_cert(ep, reload)` now takes a
    `n00b_quic_cert_reload_t` instead of required kwargs.
- DNS providers:
  - Cloudflare, Route53, and GCP constructors now take provider-specific
    config structs. Route53 no longer exposes an access-key / secret-key /
    token / zone positional train.
- RPC channel:
  - `n00b_rpc_channel_new(spec)` groups the H3 client and authority, with
    `scheme` defaulting to `https`.

## Left As-Is

- Frame, QPACK, CBOR, framer, and byte-copy APIs remain positional. They are
  low-level codec primitives where `(buffer, bytes, length)` ordering is
  conventional and extra descriptor structs would add ceremony.
- RPC registration helpers remain as generated-runtime entry points. Manual
  callers can still use the specific registration functions, and ncc's
  generated code does not need another descriptor layer yet.
- Metrics registration keeps `(registry, name, help)` positional with labels
  as `_kargs`; that is still readable and matches the Prometheus concept.
- Low-level HTTP route/spec/discovery functions remain public escape hatches.
  Service code should use the descriptor helpers. OpenAPI discovery still
  includes low-level routes as minimal operations; the helpers add metadata
  instead of acting as a visibility filter.
