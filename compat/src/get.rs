//! TIFFGetField: what the getter of a tag writes through each pointer the
//! caller passes. C reads the pointers with their exact types and writes
//! the values; the widths, counts and stored lengths are checked here.
use super::*;
use fields::Shape;
use schema::kind;
use verified_tiff::adapter_meta as checked;

/// The C types of the values, as `bridge.h` numbers them; the verified
/// `scalar_type` gives the type of each scalar.
pub const U16: u32 = 2;
pub const U32: u32 = 4;
pub const I32: u32 = 5;
pub const U64: u32 = 6;
pub const FLOAT: u32 = 8;
pub const DOUBLE: u32 = 9;
pub const POINTER: u32 = 10;

/// One value C writes through, or reads from, a caller's argument.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Value {
    pub kind: u32,
    pub bits: u64,
    pub pointer: *const c_void,
}
/// The values of a call, as many as the caller passes arguments for it.
#[repr(C)]
pub struct Plan {
    pub count: usize,
    pub value: [Value; 3],
}
const _: () = assert!(std::mem::size_of::<Value>() == 24 && std::mem::size_of::<Plan>() == 80);
const NONE: Value = Value {
    kind: 0,
    bits: 0,
    pointer: ptr::null(),
};
impl Plan {
    pub fn of(values: &[Value]) -> Self {
        let mut v = Self {
            count: values.len(),
            value: [NONE; 3],
        };
        v.value[..values.len()].copy_from_slice(values);
        v
    }
}
pub fn word(kind: u32, bits: u64) -> Value {
    Value {
        kind,
        bits,
        pointer: ptr::null(),
    }
}
fn pointer<T>(p: *const T) -> Value {
    Value {
        kind: POINTER,
        bits: 0,
        pointer: p.cast(),
    }
}
/// A value that must fit the C type of a TIFF integer type.
fn fitting(v: u64, kind: u16) -> R<u64> {
    checked::convert_integer(v, kind::LONG8, kind).map_err(|_| (INVALID, "metadata width mismatch"))
}

/// The tags whose getters write one value of a fixed type.
const LONGS: [u16; 8] = [
    tag::SUBFILETYPE,
    tag::IMAGEWIDTH,
    tag::IMAGELENGTH,
    tag::ROWSPERSTRIP,
    tag::TILEWIDTH,
    tag::TILELENGTH,
    tag::GROUP3OPTIONS,
    tag::GROUP4OPTIONS,
];
const SHORTS: [u16; 18] = [
    tag::THRESHHOLDING,
    tag::CELLWIDTH,
    tag::CELLLENGTH,
    tag::MINSAMPLEVALUE,
    tag::MAXSAMPLEVALUE,
    tag::INKSET,
    tag::NUMBEROFINKS,
    tag::YCBCRPOSITIONING,
    tag::BITSPERSAMPLE,
    tag::COMPRESSION,
    tag::PHOTOMETRIC,
    tag::FILLORDER,
    tag::ORIENTATION,
    tag::SAMPLESPERPIXEL,
    tag::PLANARCONFIG,
    tag::RESOLUTIONUNIT,
    tag::PREDICTOR,
    tag::SAMPLEFORMAT,
];
/// The ASCII tags of the main directory. InkNames has a counted setter,
/// but its getter takes only a `char **`.
const STRINGS: [u16; 12] = [
    tag::INKNAMES,
    tag::DOCUMENTNAME,
    tag::IMAGEDESCRIPTION,
    tag::MAKE,
    tag::MODEL,
    tag::PAGENAME,
    tag::SOFTWARE,
    tag::DATETIME,
    tag::ARTIST,
    tag::HOSTCOMPUTER,
    tag::COPYRIGHT,
    tag::TARGETPRINTER,
];
/// The tags of two 16-bit values, got through two pointers and set as two
/// ints.
pub const PAIRS: [u16; 4] = [tag::YCBCRSUBSAMPLING, tag::PAGENUMBER, tag::HALFTONEHINTS, tag::DOTRANGE];
/// Opaque byte blocks, got with a 32-bit count.
const BLOBS: [u16; 4] = [tag::ICCPROFILE, tag::XMLPACKET, tag::PHOTOSHOP, tag::JPEGTABLES];

/// libtiff's defaults for absent fields of the generic getter.
static WHITE_POINT: [f32; 2] = [
    96.4250 / (96.4250 + 100.0 + 82.4680),
    100.0 / (96.4250 + 100.0 + 82.4680),
];
static YCBCR_COEFFICIENTS: [f32; 3] = [0.299, 0.587, 0.114];
static STO_NITS: f64 = 1.0;

impl Handle {
    /// A scalar integer field, defaulted as TIFFGetFieldDefaulted does.
    pub fn scalar(&mut self, key: u32, defaulted: bool) -> R<Option<u64>> {
        let m = &self.meta;
        let short = u16::try_from(key).ok();
        let def = match short {
            Some(
                tag::BITSPERSAMPLE
                | tag::FILLORDER
                | tag::ORIENTATION
                | tag::SAMPLESPERPIXEL
                | tag::PLANARCONFIG
                | tag::PREDICTOR
                | tag::SAMPLEFORMAT
                | tag::COMPRESSION
                | tag::THRESHHOLDING
                | tag::INKSET
                | tag::YCBCRPOSITIONING,
            ) => Some(1),
            Some(tag::RESOLUTIONUNIT) => Some(2),
            Some(tag::GROUP3OPTIONS | tag::GROUP4OPTIONS | tag::SUBFILETYPE | tag::MINSAMPLEVALUE) => Some(0),
            Some(tag::MAXSAMPLEVALUE) => Some((1u64 << m.bits.min(16)) - 1),
            Some(tag::NUMBEROFINKS) => Some(4),
            Some(tag::ROWSPERSTRIP) => Some(u32::MAX as u64),
            _ => None,
        };
        let logluv_native = m.compression == 34676 && m.photo == 32845 && !self.logluv16;
        if logluv_native && matches!(short, Some(tag::BITSPERSAMPLE | tag::SAMPLEFORMAT)) {
            return Ok(Some(if short == Some(tag::BITSPERSAMPLE) { 32 } else { 3 }));
        }
        if key == tag::JPEGCOLORMODE {
            return Ok(Some(self.jpeg_rgb.into()));
        }
        if key == tag::SGILOGDATAFMT {
            return Ok(Some(self.logluv16.into()));
        }
        if short == Some(tag::ROWSPERSTRIP) && m.rows > 0 {
            if !m.rows_set && !defaulted {
                return Ok(None);
            }
            return Ok(Some(m.rows as u64));
        }
        let Some(t) = short.and_then(|k| find(&m.tags, k)) else {
            // libtiff's directory reader always sets these two.
            let implied = !self.writing
                && self.directory_kind == fields::MAIN
                && matches!(short, Some(tag::COMPRESSION | tag::PLANARCONFIG));
            return Ok(if defaulted || implied { def } else { None });
        };
        value(&self.data, t, 0, m.le).map(Some)
    }
    /// An array the directory reader keeps, with its length: the strile
    /// offsets or byte counts, ExtraSamples, which is always present, or
    /// SubIFD.
    fn array(&mut self, key: u16) -> R<Option<(*const c_void, usize)>> {
        let m = &mut self.meta;
        let set = |k| find(&m.tags, k).is_some();
        let offsets = matches!(key, tag::STRIPOFFSETS | tag::TILEOFFSETS);
        let counts = matches!(key, tag::STRIPBYTECOUNTS | tag::TILEBYTECOUNTS);
        let present = if offsets {
            set(tag::STRIPOFFSETS) || set(tag::TILEOFFSETS)
        } else if counts {
            set(tag::STRIPBYTECOUNTS) || set(tag::TILEBYTECOUNTS)
        } else {
            key == tag::EXTRASAMPLES || set(key)
        };
        if !present {
            return Ok(None);
        }
        if offsets {
            m.offsets.materialize(&mut self.source)?;
        }
        if counts {
            m.counts.materialize(&mut self.source)?;
        }
        let (at, n): (*const c_void, usize) = match key {
            _ if offsets => (m.offsets.values.as_ptr().cast(), m.offsets.len()),
            _ if counts => (m.counts.values.as_ptr().cast(), m.counts.len()),
            tag::EXTRASAMPLES => (m.extra.as_ptr().cast(), m.extra.len()),
            tag::SUBIFD => (m.subifds.as_ptr().cast(), m.subifds.len()),
            _ => return Err((NOTIMPL, "metadata array not implemented")),
        };
        Ok(Some((if n == 0 { ptr::null() } else { at }, n)))
    }
    /// A field's stored type, count and bytes, if it has them.
    fn bytes(&self, key: u16) -> R<Option<(u16, usize, *const u8)>> {
        let Some(t) = find(&self.meta.tags, key) else {
            return Ok(None);
        };
        if t.remote.is_some() {
            return Err((NOTIMPL, "use strile accessor for deferred arrays"));
        }
        let bytes = self.data.bytes(&t);
        checked::payload(bytes.len(), t.at, t.count, t.kind).map_err(err)?;
        Ok(Some((t.kind, t.count, bytes[t.at..].as_ptr())))
    }
    fn real(&self, key: u16) -> R<Option<f32>> {
        let Some(t) = find(&self.meta.tags, key) else {
            return Ok(None);
        };
        if t.count != 1 {
            return Err((INVALID, "scalar real field count"));
        }
        let (s, le) = (self.data.bytes(&t), self.meta.le);
        Ok(Some(match t.kind {
            kind::RATIONAL => {
                let (a, b) = (u32at(s, t.at, le)?, u32at(s, t.at + 4, le)?);
                if b == 0 {
                    return Err((INVALID, "zero rational denominator"));
                }
                a as f32 / b as f32
            }
            kind::FLOAT => f32::from_bits(u32at(s, t.at, le)?),
            kind::DOUBLE => {
                f64::from_bits(verified_tiff::native_samples::read_word(s, t.at, 8, le).map_err(err)?) as f32
            }
            _ => return Err((NOTIMPL, "real field type not implemented")),
        }))
    }
    fn pair(&self, key: u16, defaulted: bool) -> R<Option<[u16; 2]>> {
        let Some(t) = find(&self.meta.tags, key) else {
            return Ok((defaulted && key == tag::YCBCRSUBSAMPLING).then_some([2, 2]));
        };
        if t.count != 2 {
            return Err((INVALID, "pair field count"));
        }
        let half = |i| u16::try_from(value(&self.data, t, i, self.meta.le)?).map_err(|_| (INVALID, "pair overflow"));
        Ok(Some([half(0)?, half(1)?]))
    }
    /// A 16-bit main directory field, defaulted, or `fallback`.
    pub fn defaulted16(&mut self, key: u16, fallback: u16) -> R<u16> {
        Ok(match self.get(key.into(), true)? {
            Some(v) if v.count == 1 && v.value[0].kind == U16 => v.value[0].bits as u16,
            _ => fallback,
        })
    }
    /// A ColorMap's or TransferFunction's channel pointers, if it has the
    /// length the bit depth implies. Like libtiff, a defaulted
    /// TransferFunction that is absent or of another length is its
    /// default curve.
    fn channels(&mut self, key: u16, defaulted: bool) -> R<Option<Plan>> {
        let bits = self.defaulted16(tag::BITSPERSAMPLE, 1)?;
        let samples = self.defaulted16(tag::SAMPLESPERPIXEL, 1)?;
        let extras = self.array(tag::EXTRASAMPLES)?.map_or(0, |(_, n)| n);
        let extras = fitting(extras as u64, kind::SHORT)? as u16;
        let colormap = key == tag::COLORMAP;
        let layout = |n: usize| checked::channels(colormap, n, bits, samples, extras).ok();
        let (base, n) = match self.typed(key.into(), kind::SHORT)? {
            Some((base, n)) if layout(n).is_some() => (base, n),
            _ if !colormap && defaulted && bits <= 16 => self.default_transfer(1usize << bits)?,
            _ => return Ok(None),
        };
        let Some(c) = layout(n) else { return Ok(None) };
        // Each channel lies within the n entries, as the layout proves.
        let channel = |i: usize| pointer(unsafe { base.cast::<u16>().add(i * c.stride) });
        Ok(Some(Plan::of(&[channel(0), channel(1), channel(2)][..c.parts])))
    }
    /// libtiff's default for an absent field of the generic getter.
    fn generic_default(&mut self, key: u32) -> R<Option<(*const u8, usize)>> {
        let Ok(key) = u16::try_from(key) else { return Ok(None) };
        Ok(match key {
            tag::WHITEPOINT => Some((WHITE_POINT.as_ptr().cast(), 2)),
            tag::YCBCRCOEFFICIENTS => Some((YCBCR_COEFFICIENTS.as_ptr().cast(), 3)),
            tag::REFERENCEBLACKWHITE => {
                let bits = self.defaulted16(tag::BITSPERSAMPLE, 1)?;
                let photometric = self.defaulted16(tag::PHOTOMETRIC, 1)?;
                let white = (2f64.powi(bits.into()) - 1.) as f32;
                let mut r = [0., white, 0., white, 0., white];
                if photometric == 6 {
                    // YCbCr's reference black and white.
                    r = [0., 255., 128., 255., 128., 255.];
                }
                self.default_reference = r;
                Some((self.default_reference.as_ptr().cast(), 6))
            }
            tag::STONITS => Some((ptr::addr_of!(STO_NITS).cast(), 1)),
            _ => None,
        })
    }
    /// The getter of a field described by its registered shape.
    fn get_shaped(&mut self, tag: u32, defaulted: bool) -> R<Option<Plan>> {
        let Some(f): Option<Shape> = self.resolve(tag)? else {
            return Ok(None);
        };
        if f.size == 0 {
            // A convention special to a tag the main directory getter handles.
            let n = u16::try_from(tag)
                .ok()
                .and_then(|k| self.bytes(k).ok().flatten())
                .map_or(0, |b| b.1);
            if n == 0 {
                return Ok(None);
            }
            return Err((NOTIMPL, "special field convention not implemented"));
        }
        let stored = self.typed(tag, f.host)?;
        let Some((data, n)) = (match stored {
            None if defaulted => self.generic_default(tag)?,
            v => v,
        }) else {
            return Ok(None);
        };
        if f.size != container::field_size(f.host) {
            return Err((INVALID, "field schema/count mismatch"));
        }
        let plan = checked::get_plan(f.host, f.read_count, f.pass_count != 0, f.count_size, f.array, n);
        let data_or_null = pointer(if n == 0 { ptr::null() } else { data });
        Ok(Some(match plan {
            0 => return Err((INVALID, "field schema/count mismatch")),
            3 => Plan::of(&[word(U16, n as u64), data_or_null]),
            4 => Plan::of(&[word(U32, n as u64), data_or_null]),
            2 => Plan::of(&[pointer(data)]),
            _ => {
                let Some(c) = checked::scalar_type(f.host) else {
                    return Ok(None);
                };
                let mut b = [0u8; 8];
                unsafe { ptr::copy_nonoverlapping(data, b.as_mut_ptr(), f.size) };
                let bits = match f.size {
                    1 => b[0].into(),
                    2 => u16::from_ne_bytes([b[0], b[1]]).into(),
                    4 => u32::from_ne_bytes([b[0], b[1], b[2], b[3]]).into(),
                    _ => u64::from_ne_bytes(b),
                };
                Plan::of(&[word(c, bits)])
            }
        }))
    }
    /// TIFFGetField, or TIFFGetFieldDefaulted with `defaulted`.
    pub fn get(&mut self, tag: u32, defaulted: bool) -> R<Option<Plan>> {
        if self.directory_kind != fields::MAIN {
            return self.get_shaped(tag, defaulted);
        }
        if tag == tag::JPEGCOLORMODE || tag == tag::SGILOGDATAFMT {
            let v = self.scalar(tag, defaulted)?;
            return v
                .map(|v| Ok(Plan::of(&[word(I32, fitting(v, kind::SLONG)?)])))
                .transpose();
        }
        let Ok(key) = u16::try_from(tag) else {
            return self.get_shaped(tag, defaulted);
        };
        if LONGS.contains(&key) || SHORTS.contains(&key) || matches!(key, tag::EXIFIFD | tag::GPSIFD) {
            let Some(v) = self.scalar(tag, defaulted)? else {
                return Ok(None);
            };
            let value = match key {
                tag::EXIFIFD | tag::GPSIFD => word(U64, v),
                _ if LONGS.contains(&key) => word(U32, fitting(v, kind::LONG)?),
                _ => word(U16, fitting(v, kind::SHORT)?),
            };
            return Ok(Some(Plan::of(&[value])));
        }
        if STRINGS.contains(&key) {
            let Some((stored, _, p)) = self.bytes(key)? else {
                return Ok(None);
            };
            if stored != kind::ASCII {
                return Err((INVALID, "metadata width mismatch"));
            }
            return Ok(Some(Plan::of(&[pointer(p)])));
        }
        if BLOBS.contains(&key) {
            let Some((stored, n, p)) = self.bytes(key)? else {
                return Ok(None);
            };
            if !matches!(stored, kind::BYTE | kind::UNDEFINED) || n > u32::MAX as usize {
                return Err((INVALID, "metadata width mismatch"));
            }
            return Ok(Some(Plan::of(&[word(U32, n as u64), pointer(p)])));
        }
        Ok(match key {
            tag::STRIPOFFSETS | tag::STRIPBYTECOUNTS | tag::TILEOFFSETS | tag::TILEBYTECOUNTS => self
                .array(key)?
                .filter(|(_, n)| *n != 0)
                .map(|(p, _)| Plan::of(&[pointer(p)])),
            tag::EXTRASAMPLES => {
                // Extra samples implied by the photometric are only defaulted.
                if !defaulted && find(&self.meta.tags, key).is_none() {
                    return Ok(None);
                }
                let (p, n) = self.array(key)?.ok_or((5, "missing ExtraSamples"))?;
                Some(Plan::of(&[word(U16, fitting(n as u64, kind::SHORT)?), pointer(p)]))
            }
            tag::SUBIFD => match self.array(key)?.filter(|(_, n)| *n != 0) {
                Some((p, n)) => Some(Plan::of(&[word(U16, fitting(n as u64, kind::SHORT)?), pointer(p)])),
                None => None,
            },
            tag::COLORMAP | tag::TRANSFERFUNCTION => self.channels(key, defaulted)?,
            tag::XRESOLUTION | tag::YRESOLUTION | tag::XPOSITION | tag::YPOSITION => {
                self.real(key)?.map(|v| Plan::of(&[word(FLOAT, v.to_bits().into())]))
            }
            _ if PAIRS.contains(&key) => self
                .pair(key, defaulted)?
                .map(|[a, b]| Plan::of(&[word(U16, a.into()), word(U16, b.into())])),
            _ => self.get_shaped(tag, defaulted)?,
        })
    }
}

/// TIFFGetField's values for `tag`: 1 with them, 0 when the field has none,
/// -1 on failure.
#[no_mangle]
pub unsafe extern "C" fn vt_get(p: *mut c_void, tag: u32, defaulted: i32, out: *mut Plan) -> i32 {
    guard(-1, || {
        if out.is_null() {
            return Err((INVALID, "null getter output"));
        }
        match handle(p)?.get(tag, defaulted != 0)? {
            Some(v) => {
                *out = v;
                Ok(1)
            }
            None => Ok(0),
        }
    })
}
/// A scalar integer field, for TIFFIsUpSampled.
#[no_mangle]
pub unsafe extern "C" fn vt_scalar(p: *mut c_void, key: u32, defaulted: i32, out: *mut u64) -> i32 {
    guard(-1, || {
        if out.is_null() {
            return Err((INVALID, "null metadata output"));
        }
        match handle(p)?.scalar(key, defaulted != 0)? {
            Some(v) => {
                *out = v;
                Ok(1)
            }
            None => Ok(0),
        }
    })
}
