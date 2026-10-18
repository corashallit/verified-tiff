//! Reads into the caller's buffers: scanlines, strips, tiles, raw segments,
//! RGBA rasters and segments of the caller's own encoded bytes. Each read
//! decodes into the handle, then copies the bytes the caller asked for in
//! one checked commit, so the caller's buffer is unchanged on failure.
use super::*;
use decoder::Read;
use std::mem::MaybeUninit;
use verified_tiff::adapter_bounds as bounds;

/// The reads of `vt_read_into`, numbered as in bridge.h.
const SCANLINE: u32 = 0;
const ENCODED_STRIP: u32 = 1;
const ENCODED_TILE: u32 = 2;
const RAW_STRIP: u32 = 3;
const RAW_TILE: u32 = 4;
/// The rasters of `vt_rgba_into`, numbered as in bridge.h.
const RGBA_IMAGE: u32 = 0;
const RGBA_STRIP: u32 = 1;
const RGBA_TILE: u32 = 2;

/// Copies `src` to offset `at` of the caller's `capacity` bytes at `dst`,
/// which may be uninitialized. The checked core proves the exact copy and
/// the unchanged frame; overlap is rejected before references exist.
unsafe fn commit(dst: *mut u8, capacity: usize, at: usize, src: &[u8]) -> R<()> {
    let n = src.len();
    if capacity > isize::MAX as usize || !verified_tiff::checked_range(capacity, at, n) {
        return Err((LIMIT, "output commit extent"));
    }
    if n == 0 {
        return Ok(());
    }
    if dst.is_null() {
        return Err((INVALID, "null output commit"));
    }
    let end = (dst as usize)
        .checked_add(capacity)
        .ok_or((INVALID, "destination address wrap"))?;
    let source = src.as_ptr_range();
    if (dst as usize) < source.end as usize && (source.start as usize) < end {
        return Err((INVALID, "overlapping output commit"));
    }
    // MaybeUninit permits C's ordinary uninitialized malloc buffers. The
    // checked loop writes every byte before it can be treated as initialized.
    let output = std::slice::from_raw_parts_mut(dst.add(at).cast::<MaybeUninit<u8>>(), n);
    verified_tiff::adapter_bounds::commit_uninit(src, output).map_err(err)
}

/// The rendered bytes of the last read.
fn rendered(h: &Handle) -> R<&[u8]> {
    h.rendered.as_deref().ok_or((5, "missing read output"))
}

/// Reads scanline `a` of sample `b`, or segment `a`, into the caller's
/// `size` bytes at `buf`, or for a size of -1 as many bytes as the read
/// has. Like libtiff, a size of 0 only checks the segment. Gives the bytes
/// read, or -1.
#[no_mangle]
pub unsafe extern "C" fn vt_read_into(p: *mut c_void, op: u32, a: u32, b: u32, buf: *mut u8, size: i64) -> i64 {
    guard(-1, || {
        let h = handle(p)?;
        let tiled = h.meta.tiled;
        match op {
            RAW_STRIP if tiled => return Err((INVALID, "not a striped image")),
            RAW_TILE if !tiled => return Err((INVALID, "not a tiled image")),
            _ => {}
        }
        if size < -1 || buf.is_null() && size != 0 {
            return Err((INVALID, "invalid destination/size"));
        }
        let n = if size == 0 {
            let layout = op == ENCODED_STRIP && tiled || op == ENCODED_TILE && !tiled;
            if a as usize >= h.meta.offsets.len() || layout {
                return Err((INVALID, "invalid segment/layout"));
            }
            0
        } else {
            if h.meta.width == 0 {
                return Err((INVALID, "no selected image directory"));
            }
            let (a, b) = (a as usize, b as usize);
            let read = match op {
                SCANLINE => Read::Scanline { row: a, sample: b },
                ENCODED_STRIP | ENCODED_TILE => Read::Segment {
                    index: a,
                    tiled: op == ENCODED_TILE,
                },
                RAW_STRIP | RAW_TILE => Read::Raw { index: a },
                _ => return Err((INVALID, "unknown read")),
            };
            // Without a size, the read gives all of its bytes.
            let wanted = usize::try_from(size).ok();
            h.read(read, wanted.unwrap_or(usize::MAX))?;
            let out = rendered(h)?;
            let (n, capacity) = bounds::read_extent(wanted, out.len());
            commit(buf, capacity, 0, &out[..n])?;
            n
        };
        let position = match op {
            SCANLINE => Some(index::ROW),
            ENCODED_STRIP => Some(index::STRIP),
            ENCODED_TILE => Some(index::TILE),
            _ => None,
        };
        // A position the read cannot compute is forgotten.
        if let Some(kind) = position {
            if h.advance(kind, a, b).is_err() {
                h.position = [u32::MAX; 3];
            }
        }
        Ok(n as i64)
    })
}

/// The RGBA raster of TIFFReadRGBAImageOriented (`a` by `b` pixels),
/// TIFFReadRGBAStrip (from row `a`) or TIFFReadRGBATile (at `a`, `b`).
/// Like libtiff, an image raster taller than the image holds it in its
/// last rows.
#[no_mangle]
pub unsafe extern "C" fn vt_rgba_into(p: *mut c_void, op: u32, a: u32, b: u32, orientation: u32, out: *mut u32) -> i32 {
    guard(0, || {
        let h = handle(p)?;
        if out.is_null() {
            return Err((INVALID, "null raster"));
        }
        if h.meta.width == 0 {
            return Err((INVALID, "no selected image directory"));
        }
        let (a, b) = (a as usize, b as usize);
        let (read, below) = match op {
            RGBA_IMAGE => {
                let orientation = orientation as usize;
                let below = product(b.saturating_sub(h.meta.height), a)?;
                (
                    Read::RgbaImage {
                        width: a,
                        height: b,
                        orientation,
                    },
                    below,
                )
            }
            RGBA_STRIP => (Read::RgbaStrip { row: a }, 0),
            RGBA_TILE => (Read::RgbaTile { x: a, y: b }, 0),
            _ => return Err((INVALID, "unknown RGBA read")),
        };
        h.read(read, usize::MAX)?;
        let pixels = rendered(h)?;
        let (at, capacity) =
            bounds::raster_frame(below, pixels.len()).map_err(|_| (LIMIT, "raster extent overflow"))?;
        commit(out.cast(), capacity, at, pixels)?;
        Ok(1)
    })
}

/// TIFFRGBAImageGet's `w` by `h` raster, from column `col` and row `row`
/// of an image `height` rows tall, stored in orientation `source` and
/// drawn in `orientation`. Rows below the image stay unwritten, at the
/// raster's start.
#[no_mangle]
pub unsafe extern "C" fn vt_region_into(
    p: *mut c_void,
    col: i32,
    row: i32,
    height: u32,
    w: u32,
    h: u32,
    source: u32,
    orientation: u32,
    out: *mut u32,
) -> i32 {
    guard(0, || {
        let image = handle(p)?;
        let (Ok(x), Ok(y)) = (u32::try_from(col), u32::try_from(row)) else {
            return Err((INVALID, "invalid RGBA region/output"));
        };
        if out.is_null() || y >= height {
            return Err((INVALID, "invalid RGBA region/output"));
        }
        let rows = h.min(height - y);
        let below = product((h - rows) as usize, w as usize)?;
        let m = &image.meta;
        let (x, y, w, rows) = (x as usize, y as usize, w as usize, rows as usize);
        if x >= m.width || y >= m.height || w > m.width - x || rows > m.height - y {
            return Err((INVALID, "RGBA region outside image"));
        }
        // libtiff applies no flip for a source orientation it does not know.
        let source = if (1..=8).contains(&source) { source } else { orientation };
        image.rgba_from(x, y, w, rows, source as usize, orientation as usize)?;
        let pixels = rendered(image)?;
        let (at, capacity) = bounds::raster_frame(below, pixels.len()).map_err(|_| (LIMIT, "RGBA raster overflow"))?;
        commit(out.cast(), capacity, at, pixels)?;
        Ok(1)
    })
}

/// TIFFReadFromUserBuffer: decodes the caller's `n` encoded bytes as
/// segment `k`, and gives the first `wanted` bytes.
#[no_mangle]
pub unsafe extern "C" fn vt_user_buffer_into(
    p: *mut c_void,
    k: u32,
    input: *const u8,
    n: i64,
    output: *mut u8,
    wanted: i64,
) -> i32 {
    guard(0, || {
        let h = handle(p)?;
        let (Ok(n), Ok(wanted)) = (usize::try_from(n), usize::try_from(wanted)) else {
            return Err((INVALID, "invalid user buffer"));
        };
        if input.is_null() && n != 0 || output.is_null() && wanted != 0 {
            return Err((INVALID, "invalid user buffer"));
        }
        h.native_ok()?;
        if n > h.limits.max_input || wanted > h.limits.max_output {
            return Err((LIMIT, "user buffer limit"));
        }
        let mut encoded = Buf::new(n, &h.source.mem)?;
        if n != 0 {
            encoded.extend(std::slice::from_raw_parts(input, n))?;
        }
        let mut budget = Budget { remaining: h.work };
        let decoded = h.decode_segment(k as usize, encoded, &mut budget)?;
        let Some(bytes) = decoded.get(..wanted) else {
            return Err((INVALID, "user output exceeds decoded segment"));
        };
        commit(output, wanted, 0, bytes)?;
        Ok(1)
    })
}

/// TIFFReadBufferSetup: stage reads in the caller's `n` bytes, or in `n`
/// bytes of the handle's own.
#[no_mangle]
pub unsafe extern "C" fn vt_read_buffer(p: *mut c_void, buffer: *mut u8, n: i64) -> i32 {
    guard(0, || {
        let h = handle(p)?;
        let n = usize::try_from(n)
            .ok()
            .filter(|&n| n != 0)
            .ok_or((INVALID, "invalid read buffer size"))?;
        let staging = if buffer.is_null() {
            storage::ReadBuffer::Owned(Buf::zeros(n, h.limits.max_input, &h.source.mem)?)
        } else {
            storage::ReadBuffer::Borrowed(buffer, n)
        };
        h.source.read_buffer = Some(staging);
        h.stream = None;
        h.cache = None;
        Ok(1)
    })
}

/// TIFFRawStripSize: segment `k`'s byte count, which must fit a tmsize_t.
#[no_mangle]
pub unsafe extern "C" fn vt_raw_size(p: *mut c_void, k: u32) -> i64 {
    guard(-1, || {
        let h = handle(p)?;
        let n = h.meta.counts.get(&mut h.source, k as usize)?;
        i64::try_from(n).map_err(|_| (LIMIT, "raw strip size overflow"))
    })
}

/// The fields of a TIFFRGBAImage that TIFFRGBAImageBegin fills.
#[repr(C)]
pub struct Rgba {
    width: u32,
    height: u32,
    orientation: u16,
    bits: u16,
    samples: u16,
    photometric: u16,
    alpha: u16,
    contig: u16,
    colormap: [*const u16; 3],
}
const _: () = assert!(std::mem::size_of::<Rgba>() == 48);

/// TIFFRGBAImageBegin's description of the image, from the fields
/// TIFFGetFieldDefaulted gives.
#[no_mangle]
pub unsafe extern "C" fn vt_rgba_begin(p: *mut c_void, out: *mut Rgba) -> i32 {
    guard(0, || {
        let h = handle(p)?;
        let out = out.as_mut().ok_or((INVALID, "null RGBA state"))?;
        h.rgba_ok()?;
        let samples = h.defaulted16(tag::SAMPLESPERPIXEL, 0)?;
        // Without the tag, libtiff guesses RGB or grayscale from the samples.
        let photometric = match h.field16(tag::PHOTOMETRIC, false)? {
            Some(v) => v,
            None => u16::try_from(h.meta.photo).map_err(|_| (INVALID, "photometric exceeds 16 bits"))?,
        };
        // An unspecified first extra sample beyond three is associated alpha.
        let alpha = match h.meta.extra.first() {
            Some(&0) if samples > 3 => 1,
            Some(&v) => v,
            None => 0,
        };
        let mut colormap = [ptr::null(); 3];
        if photometric == 3 {
            if let Some(map) = h.get(tag::COLORMAP.into(), false)? {
                for (c, v) in colormap.iter_mut().zip(&map.value[..map.count]) {
                    *c = v.pointer.cast();
                }
            }
        }
        *out = Rgba {
            width: u32::try_from(h.meta.width).map_err(|_| (INVALID, "image width exceeds 32 bits"))?,
            height: u32::try_from(h.meta.height).map_err(|_| (INVALID, "image height exceeds 32 bits"))?,
            orientation: h.defaulted16(tag::ORIENTATION, 0)?,
            bits: h.defaulted16(tag::BITSPERSAMPLE, 0)?,
            samples,
            photometric,
            alpha,
            contig: (h.meta.planar == 1).into(),
            colormap,
        };
        Ok(1)
    })
}

/// TIFFIsUpSampled: whether YCbCr JPEG is decoded as RGB.
#[no_mangle]
pub unsafe extern "C" fn vt_upsampled(p: *mut c_void) -> i32 {
    guard(0, || {
        let h = handle(p)?;
        Ok((h.scalar(tag::PHOTOMETRIC.into(), true)? == Some(6) && h.jpeg_rgb).into())
    })
}
