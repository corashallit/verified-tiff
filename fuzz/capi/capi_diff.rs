//! libFuzzer entry point comparing this library with the pinned libtiff.
#![no_main]
use libfuzzer_sys::fuzz_target;
extern crate verified_tiff_capi;
extern "C" {
    fn vtiff_diff_one(data: *const u8, size: usize) -> i32;
}
fuzz_target!(|data: &[u8]| {
    unsafe { vtiff_diff_one(data.as_ptr(), data.len()) };
});
