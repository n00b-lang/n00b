//! S3 wrappers.
//!
//! Phase 3 of WP-003 explicitly scopes S3 out of the
//! bounded-payload dead-letter (Phase 3e: "no S3 yet"). The crate
//! declares `aws-sdk-s3` as a dependency so the shim cdylib pulls
//! in the S3 client code once; consumers that don't need S3 today
//! still get the SDK loaded in libn00b_aws's process. This module
//! starts as a stub and grows op-by-op as concrete consumers
//! arrive — `PutObject` + `GetObject` first when Phase 3e revisits
//! dead-letter storage.

use std::os::raw::c_char;

/// Returns the version string of the underlying `aws-sdk-s3` crate.
/// Lives behind a no-op symbol so demangle/cbindgen pull in the S3
/// dependency closure during build and confirm it links cleanly.
#[no_mangle]
pub extern "C" fn n00b_aws_shim_s3_sdk_version() -> *const c_char {
    static VERSION: &[u8] = concat!(
        "aws-sdk-s3 ", env!("CARGO_PKG_VERSION"), "\0"
    ).as_bytes();
    VERSION.as_ptr() as *const c_char
}

/// Silence unused-crate warnings while the wrap is empty.
#[allow(dead_code)]
fn _force_sdk_link(_: aws_sdk_s3::Client) {}
