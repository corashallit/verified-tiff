//! libFuzzer entry point for the C API harness in harness.c.
#![no_main]
use libfuzzer_sys::fuzz_target;
// The harness calls the adapter's exported C symbols.
extern crate verified_tiff_capi;
extern "C" {
    fn vtiff_fuzz_one(data: *const u8, size: usize) -> i32;
}
fuzz_target!(|data: &[u8]| {
    unsafe { vtiff_fuzz_one(data.as_ptr(), data.len()) };
});
