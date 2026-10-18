//! TIFFOpen's options: the resource limits, their defaults from the
//! environment, the mode string and whether a new handle fits the limits.
use super::*;
use std::ffi::CStr;
use verified_tiff::adapter_options as checked;

/// The values of libtiff's opaque TIFFOpenOptions, which C embeds with the
/// caller's error handlers.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Options {
    pub input: usize,
    pub output: usize,
    pub work: usize,
    pub single: usize,
    pub cumulative: usize,
    pub warn_unknown: i32,
    /// Whether a negative allocation limit was set.
    pub unsupported: i32,
    /// One bit for each limit whose environment variable is malformed.
    pub invalid_env: u32,
}
const _: () = assert!(std::mem::size_of::<Options>() == 56);

/// The adapter's open flags, as the verified `mode_flags` gives them.
pub const CHOP: u32 = 1;
/// Read only the header, as libtiff's `h` mode does.
pub const HEADER: u32 = 2;
pub const WRITE: u32 = 4;
pub const BIGTIFF: u32 = 8;
pub const BIGENDIAN: u32 = 16;
/// Read strip and tile arrays only when used, as the `D` and `O` modes do.
pub const DEFERRED: u32 = 32;

/// Each limit's environment variable and default, in the order of
/// `Options`. The work limit may exceed the largest object size.
const VARIABLES: [(&CStr, usize); 5] = [
    (c"VTIFF_MAX_FILE_BYTES", isize::MAX as usize),
    (c"VTIFF_MAX_BUFFER_BYTES", 256 << 20),
    (c"VTIFF_MAX_WORK", 1_000_000_000),
    (c"VTIFF_MAX_SINGLE_ALLOC", 256 << 20),
    (c"VTIFF_MAX_TOTAL_ALLOC", 512 << 20),
];
const WORK: usize = 2;
/// The bits of `invalid_env` that VTIFFOpenOptionsSetLimits and the two
/// allocation limit setters replace.
const STREAM_LIMITS: u32 = 7;
const SINGLE_LIMIT: u32 = 8;
const CUMULATIVE_LIMIT: u32 = 16;
/// libtiff's file names are paths, which Linux limits to 4095 bytes.
const MAX_NAME: usize = 4096;

/// The C library's getenv, which C passes in.
type Getenv = unsafe extern "C" fn(*const c_char) -> *mut c_char;

fn object_size(v: u64) -> R<usize> {
    usize::try_from(v)
        .ok()
        .filter(|&v| v <= isize::MAX as usize)
        .ok_or((INVALID, "invalid options/limits"))
}

/// The default options, with limits from the environment.
#[no_mangle]
pub unsafe extern "C" fn vt_options_default(o: *mut Options, getenv: Getenv) {
    guard((), || {
        let o = o.as_mut().ok_or((INVALID, "null options"))?;
        let mut limits = [0; 5];
        let mut invalid_env = 0;
        for (i, (name, fallback)) in VARIABLES.iter().enumerate() {
            let text = getenv(name.as_ptr());
            let value = if text.is_null() {
                Some(*fallback)
            } else {
                checked::decimal(CStr::from_ptr(text).to_bytes())
            };
            if value.filter(|&v| i == WORK || v <= isize::MAX as usize).is_none() {
                invalid_env |= 1 << i;
            }
            limits[i] = value.unwrap_or(*fallback);
        }
        let [input, output, work, single, cumulative] = limits;
        *o = Options {
            input,
            output,
            work,
            single,
            cumulative,
            warn_unknown: 0,
            unsupported: 0,
            invalid_env,
        };
        Ok(())
    })
}
/// VTIFFOpenOptionsSetLimits: the input, output and work limits.
#[no_mangle]
pub unsafe extern "C" fn vt_options_limits(o: *mut Options, input: u64, output: u64, work: u64) -> i32 {
    guard(0, || {
        let o = o.as_mut().ok_or((INVALID, "invalid options/limits"))?;
        let (input, output) = (object_size(input)?, object_size(output)?);
        o.work = usize::try_from(work).map_err(|_| (INVALID, "invalid options/limits"))?;
        o.input = input;
        o.output = output;
        o.invalid_env &= !STREAM_LIMITS;
        Ok(1)
    })
}
/// TIFFOpenOptionsSetMaxSingleMemAlloc or, with `cumulative`,
/// TIFFOpenOptionsSetMaxCumulatedMemAlloc. Zero disables the limit; a
/// negative one makes the options unusable.
#[no_mangle]
pub unsafe extern "C" fn vt_options_allocation(o: *mut Options, cumulative: i32, n: i64) -> i32 {
    guard(0, || {
        let o = o.as_mut().ok_or((INVALID, "invalid allocation limit"))?;
        let Ok(n) = usize::try_from(n) else {
            o.unsupported = 1;
            return Err((INVALID, "invalid allocation limit"));
        };
        if cumulative != 0 {
            o.cumulative = n;
            o.invalid_env &= !CUMULATIVE_LIMIT;
        } else {
            o.single = n;
            o.invalid_env &= !SINGLE_LIMIT;
        }
        Ok(1)
    })
}
/// The adapter's open flags for a libtiff mode string, or -1 for a mode
/// it does not support.
#[no_mangle]
pub unsafe extern "C" fn vt_mode_flags(mode: *const c_char) -> i64 {
    if mode.is_null() {
        return -1;
    }
    checked::mode_flags(CStr::from_ptr(mode).to_bytes()).map_or(-1, i64::from)
}
/// Whether a handle of `handle` bytes named `name` may open under `o`: the
/// options are valid, the name is shorter than 4096 bytes, and the handle
/// and a copy of the name fit the allocation limits. Gives the bytes of
/// the copy, or 0 on failure.
#[no_mangle]
pub unsafe extern "C" fn vt_admit(o: *const Options, name: *const c_char, handle: usize) -> usize {
    guard(0, || {
        let o = o.as_ref().ok_or((INVALID, "null options"))?;
        if name.is_null() {
            return Err((INVALID, "null name"));
        }
        if o.unsupported != 0 || o.invalid_env != 0 {
            return Err((INVALID, "invalid configured resource limit"));
        }
        let mut n = 0;
        while n < MAX_NAME && *name.add(n) != 0 {
            n += 1;
        }
        if n == MAX_NAME {
            return Err((INVALID, "name exceeds 4095 bytes"));
        }
        let limit = |_| (LIMIT, "handle allocation limit");
        let used = verified_tiff::adapter_bounds::claim(0, handle, o.single, o.cumulative).map_err(limit)?;
        verified_tiff::adapter_bounds::claim(used, n + 1, o.single, o.cumulative).map_err(limit)?;
        Ok(n + 1)
    })
}
