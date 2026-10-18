//! libtiff's client info, byte swapping, bit reversal and data widths.
use super::*;
use verified_tiff::adapter_bounds as bounds;

/// A pointer TIFFSetClientInfo keeps under a name.
pub struct ClientInfo {
    name: Buf<u8>,
    data: *mut c_void,
}
/// Client-info names are shorter than this.
const MAX_NAME: usize = 4096;

/// The bytes of a C name before its NUL, if they are fewer than MAX_NAME.
unsafe fn bounded<'a>(name: *const c_char) -> Option<&'a [u8]> {
    let mut n = 0;
    while n < MAX_NAME && *name.add(n) != 0 {
        n += 1;
    }
    (n < MAX_NAME).then(|| std::slice::from_raw_parts(name.cast(), n))
}

/// TIFFGetClientInfo: the pointer kept under `name`, or null.
#[no_mangle]
pub unsafe extern "C" fn vt_client_info(p: *mut c_void, name: *const c_char) -> *mut c_void {
    guard(ptr::null_mut(), || {
        let h = handle(p)?;
        if name.is_null() {
            return Ok(ptr::null_mut());
        }
        // A name too long to be kept has nothing under it.
        let Some(name) = bounded(name) else {
            return Ok(ptr::null_mut());
        };
        Ok(h.client_info
            .iter()
            .find(|c| *c.name == *name)
            .map_or(ptr::null_mut(), |c| c.data))
    })
}
/// TIFFSetClientInfo: keeps `data` under `name`, replacing what was there.
#[no_mangle]
pub unsafe extern "C" fn vt_set_client_info(p: *mut c_void, data: *mut c_void, name: *const c_char) -> i32 {
    guard(0, || {
        let h = handle(p)?;
        if name.is_null() {
            return Err((INVALID, "null client-info name"));
        }
        let name = bounded(name).ok_or((INVALID, "client-info name too long"))?;
        if let Some(c) = h.client_info.iter_mut().find(|c| *c.name == *name) {
            c.data = data;
            return Ok(1);
        }
        let mut copy = Buf::new(name.len(), &h.source.mem)?;
        copy.extend(name)?;
        h.client_info.reserve((h.client_info.len() + 1).next_power_of_two())?;
        h.client_info.push(ClientInfo { name: copy, data })?;
        Ok(1)
    })
}

/// TIFFSwab and TIFFSwabArrayOf: reverses the bytes of each of `n` values
/// of `width` bytes at `p`.
#[no_mangle]
pub unsafe extern "C" fn vt_swab(p: *mut u8, n: i64, width: usize) -> i32 {
    guard(0, || {
        let invalid = (INVALID, "invalid buffer/count");
        let n = usize::try_from(n).map_err(|_| invalid)?;
        let bytes = bounds::allocation(n, width).map_err(|_| invalid)?;
        if bytes == 0 {
            return Ok(1);
        }
        if p.is_null() {
            return Err(invalid);
        }
        for value in std::slice::from_raw_parts_mut(p, bytes).chunks_exact_mut(width) {
            value.reverse();
        }
        Ok(1)
    })
}
/// TIFFReverseBits: reverses the bits of each of the `n` bytes at `p`.
#[no_mangle]
pub unsafe extern "C" fn vt_reverse_bits(p: *mut u8, n: i64) -> i32 {
    guard(0, || {
        let invalid = (INVALID, "invalid buffer/count");
        let n = usize::try_from(n).map_err(|_| invalid)?;
        if n == 0 {
            return Ok(1);
        }
        if p.is_null() {
            return Err(invalid);
        }
        verified_tiff::native_samples::reverse_bits(std::slice::from_raw_parts_mut(p, n));
        Ok(1)
    })
}

const fn bit_table(reversed: bool) -> [u8; 256] {
    let mut table = [0; 256];
    let mut i = 0;
    while i < 256 {
        table[i] = if reversed { (i as u8).reverse_bits() } else { i as u8 };
        i += 1;
    }
    table
}
static IDENTITY: [u8; 256] = bit_table(false);
static REVERSED: [u8; 256] = bit_table(true);
/// TIFFGetBitRevTable: each byte, with its bits reversed if `reversed`.
#[no_mangle]
pub extern "C" fn vt_bit_table(reversed: i32) -> *const u8 {
    if reversed != 0 {
        REVERSED.as_ptr()
    } else {
        IDENTITY.as_ptr()
    }
}

/// TIFFDataWidth: the bytes of a value of TIFF type `kind`, or 0.
#[no_mangle]
pub extern "C" fn vt_data_width(kind: i32) -> i32 {
    u16::try_from(kind).map_or(0, |k| container::field_size(k) as i32)
}
