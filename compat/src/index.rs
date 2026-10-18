//! libtiff's strip and tile numbering, and the image position that reads
//! leave behind. The arithmetic is verified in `adapter_geometry`.
use super::*;
use verified_tiff::adapter_geometry as geometry;

/// Positions libtiff reports, and the reads that set them; `bridge.h` has
/// the same numbers.
pub const ROW: u32 = 0;
pub const STRIP: u32 = 1;
pub const TILE: u32 = 2;

/// One of the 32-bit dimensions a directory always has, or 0 without one.
fn dimension(n: usize) -> R<u32> {
    u32::try_from(n).map_err(|_| (INVALID, "image dimension exceeds 32 bits"))
}

impl Handle {
    fn separate(&self) -> bool {
        self.meta.planar == 2
    }
    fn samples(&self) -> R<u16> {
        u16::try_from(self.meta.ch).map_err(|_| (INVALID, "sample count exceeds 16 bits"))
    }
    fn compute_strip(&self, row: u32, sample: u16) -> R<u32> {
        let m = &self.meta;
        // Like libtiff, a tiled image's strips follow RowsPerStrip.
        let rows = if m.tiled { m.rows } else { m.bh };
        geometry::strip_index(
            dimension(m.height)?,
            rows as u64,
            m.offsets.len() as u64,
            self.separate(),
            self.samples()?,
            row,
            sample,
        )
        .map_err(|_| (INVALID, "invalid strip row/plane"))
    }
    /// Record a successful read of scanline `a` of sample `b` (`ROW`),
    /// strip `a` (`STRIP`) or tile `a` (`TILE`), as libtiff's current row,
    /// strip and tile.
    pub fn advance(&mut self, kind: u32, a: u32, b: u32) -> R<()> {
        let m = &self.meta;
        let (w, ht, bw, bh) = (
            dimension(m.width)?,
            dimension(m.height)?,
            dimension(m.bw)?,
            dimension(m.bh)?,
        );
        match kind {
            ROW => {
                let strip = u16::try_from(b)
                    .ok()
                    .and_then(|s| self.compute_strip(a, s).ok());
                self.position[ROW as usize] =
                    a.checked_add(1).ok_or((INVALID, "scanline row overflow"))?;
                self.position[STRIP as usize] = strip.unwrap_or(u32::MAX);
            }
            STRIP => {
                self.position[ROW as usize] = geometry::segment_row(a, ht, bh).map_err(err)?;
                self.position[STRIP as usize] = a;
            }
            TILE => {
                self.position[ROW as usize] = geometry::tile_row(a, w, ht, bw, bh).map_err(err)?;
                self.position[TILE as usize] = a;
            }
            _ => return Err((INVALID, "unknown read kind")),
        }
        Ok(())
    }
}

/// TIFFNumberOfStrips, or -1.
#[no_mangle]
pub unsafe extern "C" fn vt_strips(p: *mut c_void) -> i64 {
    guard(-1, || {
        let h = handle(p)?;
        let m = &h.meta;
        if !m.tiled && m.width != 0 {
            return Ok(m.offsets.len() as i64);
        }
        let planes = if h.separate() { h.samples()? } else { 1 };
        Ok(geometry::strips_from_rows(m.rows as u64, dimension(m.height)?, planes).into())
    })
}
#[no_mangle]
pub unsafe extern "C" fn vt_check_tile(p: *mut c_void, x: u32, y: u32, z: u32, s: u16) -> i32 {
    guard(0, || {
        let h = handle(p)?;
        let m = &h.meta;
        if !geometry::check_tile(
            dimension(m.width)?,
            dimension(m.height)?,
            h.separate(),
            h.samples()?,
            x,
            y,
            z,
            s,
        ) {
            return Err((INVALID, "invalid tile coordinates/plane"));
        }
        Ok(1)
    })
}
#[no_mangle]
pub unsafe extern "C" fn vt_compute_tile(
    p: *mut c_void,
    x: u32,
    y: u32,
    z: u32,
    s: u16,
    out: *mut u32,
) -> i32 {
    guard(0, || {
        let h = handle(p)?;
        let m = &h.meta;
        if out.is_null() {
            return Err((INVALID, "null tile output"));
        }
        *out = geometry::tile_index(
            dimension(m.width)?,
            dimension(m.height)?,
            dimension(m.bw)?,
            dimension(m.bh)?,
            h.separate(),
            h.samples()?,
            x,
            y,
            z,
            s,
        )
        .map_err(|_| (INVALID, "invalid tile coordinates/plane"))?;
        Ok(1)
    })
}
#[no_mangle]
pub unsafe extern "C" fn vt_compute_strip(p: *mut c_void, row: u32, s: u16, out: *mut u32) -> i32 {
    guard(0, || {
        let h = handle(p)?;
        if out.is_null() {
            return Err((INVALID, "null strip output"));
        }
        *out = h.compute_strip(row, s)?;
        Ok(1)
    })
}
/// The current `ROW`, `STRIP` or `TILE`.
#[no_mangle]
pub unsafe extern "C" fn vt_position(p: *mut c_void, which: u32) -> u32 {
    guard(u32::MAX, || {
        let h = handle(p)?;
        h.position
            .get(which as usize)
            .copied()
            .ok_or((INVALID, "unknown image position"))
    })
}
