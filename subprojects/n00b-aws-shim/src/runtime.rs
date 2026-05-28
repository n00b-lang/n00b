//! Process-global Tokio runtime used by every async aws-sdk-rust call
//! reachable from the shim's `extern "C"` surface.
//!
//! The AWS SDK is async-first. C callers want a synchronous shape, so
//! each operation wrapper calls `runtime().block_on(async { ... })`
//! to bridge the two. A single multi-thread runtime is cheaper than
//! constructing one per call (~ms vs ~us per request).

use once_cell::sync::Lazy;
use tokio::runtime::Runtime;

static RUNTIME: Lazy<Runtime> = Lazy::new(|| {
    // Multi-thread runtime — AWS SDK clients use the runtime for
    // hyper's connection pool, retry-backoff timers, etc. A
    // single-threaded runtime would serialise unrelated calls
    // through the same thread.
    tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        // `n00b_aws_shim_*` is the prefix on our FFI symbols; the
        // matching thread name keeps task dumps recognisable to
        // anyone debugging an n00b consumer.
        .thread_name("n00b_aws_shim")
        .build()
        .expect("n00b_aws_shim: failed to construct Tokio runtime")
});

/// Returns the process-global runtime. Cheap after first call.
pub(crate) fn runtime() -> &'static Runtime {
    &RUNTIME
}
