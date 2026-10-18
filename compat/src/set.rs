//! TIFFSetField: the C types of the arguments the setter of a tag takes,
//! and the field it stores from them. C reads each argument with the type
//! planned here; the verified `set_extent` counts the bytes read through an
//! array argument.
use super::*;
use fields::{Shape, VARIABLE2};
use get::{word, Plan, DOUBLE, I32, POINTER, U32};
use schema::kind;
use verified_tiff::adapter_meta as checked;

/// How the setter of a field takes its arguments.
#[derive(Clone, Copy)]
enum Form {
    /// JPEGCOLORMODE or SGILOGDATAFMT, an int selecting a decoding mode.
    Mode,
    /// Two 16-bit values, each passed as an int.
    Pair,
    /// Three channel arrays of 2^BitsPerSample entries.
    ColorMap,
    /// A count, then an array of that many values.
    Counted(Shape),
    /// A NUL-terminated string.
    Text(Shape),
    /// An array of the field's fixed write count.
    Fixed(Shape),
    /// One value, passed as the C type.
    Scalar(Shape, u32),
}

/// `bytes` bytes of a caller's array, which may be null when empty.
unsafe fn array<'a>(p: *const c_void, bytes: usize) -> R<&'a [u8]> {
    if bytes == 0 {
        return Ok(&[]);
    }
    if p.is_null() {
        return Err((INVALID, "null field input"));
    }
    if bytes > isize::MAX as usize {
        return Err((LIMIT, "field input size"));
    }
    Ok(std::slice::from_raw_parts(p.cast(), bytes))
}
/// A caller's string with its NUL, which must come within `limit` bytes.
unsafe fn text<'a>(p: *const c_void, limit: usize) -> R<&'a [u8]> {
    if p.is_null() {
        return Err((INVALID, "null field input"));
    }
    let p = p.cast::<u8>();
    let mut n = 0;
    while n < limit && *p.add(n) != 0 {
        n += 1;
    }
    if n == limit {
        return Err((LIMIT, "string limit"));
    }
    Ok(std::slice::from_raw_parts(p, n + 1))
}

impl Handle {
    fn set_form(&self, tag: u32) -> R<Form> {
        if tag == tag::JPEGCOLORMODE || tag == tag::SGILOGDATAFMT {
            return Ok(Form::Mode);
        }
        let main = self.directory_kind == fields::MAIN;
        let short = u16::try_from(tag).ok();
        if main && short.is_some_and(|k| get::PAIRS.contains(&k)) {
            return Ok(Form::Pair);
        }
        if main && short == Some(tag::COLORMAP) {
            return Ok(Form::ColorMap);
        }
        let f = self
            .fields
            .lookup(self.directory_kind, tag)
            .ok_or((NOTIMPL, "unregistered field"))?;
        Ok(if f.pass_count != 0 {
            Form::Counted(f)
        } else if f.host == kind::ASCII {
            Form::Text(f)
        } else if f.array || f.write_count > 1 {
            Form::Fixed(f)
        } else {
            Form::Scalar(
                f,
                checked::argument_type(f.host).ok_or((NOTIMPL, "field storage convention"))?,
            )
        })
    }
    fn form_plan(form: Form) -> Plan {
        let kinds: &[u32] = match form {
            Form::Mode => &[I32],
            Form::Pair => &[I32, I32],
            Form::ColorMap => &[POINTER, POINTER, POINTER],
            Form::Counted(f) if f.write_count == VARIABLE2 => &[U32, POINTER],
            Form::Counted(_) => &[I32, POINTER],
            Form::Text(_) | Form::Fixed(_) => &[POINTER],
            Form::Scalar(_, c) => return Plan::of(&[word(c, 0)]),
        };
        let mut plan = Plan::of(&[]);
        for &k in kinds {
            plan.value[plan.count] = word(k, 0);
            plan.count += 1;
        }
        plan
    }
    /// The C types of the arguments TIFFSetField takes for `tag`.
    pub fn set_plan(&self, tag: u32) -> R<Plan> {
        Ok(Self::form_plan(self.set_form(tag)?))
    }
    /// TIFFSetField, with the arguments C read as `set_plan` planned them.
    ///
    /// # Safety
    /// Each pointer argument must be valid for the bytes its field implies.
    pub unsafe fn set(&mut self, tag: u32, args: &Plan) -> R<()> {
        let form = self.set_form(tag)?;
        let plan = Self::form_plan(form);
        let a = args
            .value
            .get(..args.count)
            .ok_or((INVALID, "setter argument plan mismatch"))?;
        if a.len() != plan.count || a.iter().zip(&plan.value).any(|(a, p)| a.kind != p.kind) {
            return Err((INVALID, "setter argument plan mismatch"));
        }
        let key = || u16::try_from(tag).map_err(|_| (INVALID, "tag overflow"));
        match form {
            Form::Mode => self.mode(tag, a[0].bits as i32),
            Form::Pair => {
                // Like libtiff, each int is truncated to 16 bits.
                let [x, y] = [a[0].bits as u16, a[1].bits as u16].map(u16::to_ne_bytes);
                self.put_native(key()?, kind::SHORT, 2, &[x[0], x[1], y[0], y[1]])
            }
            Form::ColorMap => {
                let bits = self.defaulted16(tag::BITSPERSAMPLE, 1)?;
                let n = checked::colormap_entries(bits).map_err(|_| (INVALID, "palette bit depth"))?;
                let mut raw = Buf::new(product(n, 6)?, &self.source.mem)?;
                for v in a {
                    raw.extend(array(v.pointer, n * 2)?)?;
                }
                self.put_native(tag::COLORMAP, kind::SHORT, n * 3, &raw)
            }
            Form::Counted(f) => {
                let passed = if a[0].kind == U32 {
                    a[0].bits as u32 as i64
                } else {
                    a[0].bits as i64
                };
                let (n, bytes) =
                    checked::set_extent(f.host, f.write_count, true, passed).map_err(|_| (INVALID, "field count"))?;
                self.store(f, n, array(a[1].pointer, bytes)?)
            }
            Form::Text(f) => {
                let s = text(a[0].pointer, self.limits.max_output)?;
                self.store(f, s.len(), s)
            }
            Form::Fixed(f) => {
                let (n, bytes) = checked::set_extent(f.host, f.write_count, false, 0)
                    .map_err(|_| (NOTIMPL, "unsized array convention"))?;
                self.store(f, n, array(a[0].pointer, bytes)?)
            }
            Form::Scalar(f, _) => {
                let bits = match (a[0].kind, f.host) {
                    (DOUBLE, kind::FLOAT) => (f64::from_bits(a[0].bits) as f32).to_bits().into(),
                    _ => a[0].bits,
                };
                // The value's low bytes, so that, like libtiff, an integer is
                // truncated to the field's width.
                let w = container::field_size(f.host);
                let at = if cfg!(target_endian = "big") { 8 - w } else { 0 };
                self.store(f, 1, &bits.to_ne_bytes()[at..at + w])
            }
        }
    }
    /// Store `count` values of a field's host type from `raw`, in native
    /// byte order; a rational field is stored from floats or doubles.
    fn store(&mut self, f: Shape, count: usize, raw: &[u8]) -> R<()> {
        if matches!(f.kind, kind::RATIONAL | kind::SRATIONAL) {
            self.put_real(f.tag, f.kind, f.host, count, raw)
        } else {
            self.put_native(f.tag, f.host, count, raw)
        }
    }
}

/// The C types of TIFFSetField's arguments for `tag`.
#[no_mangle]
pub unsafe extern "C" fn vt_set_plan(p: *mut c_void, tag: u32, out: *mut Plan) -> i32 {
    guard(0, || {
        if out.is_null() {
            return Err((INVALID, "null setter plan"));
        }
        *out = handle(p)?.set_plan(tag)?;
        Ok(1)
    })
}
/// TIFFSetField with the arguments of `vt_set_plan`'s plan.
#[no_mangle]
pub unsafe extern "C" fn vt_set(p: *mut c_void, tag: u32, args: *const Plan) -> i32 {
    guard(0, || {
        if args.is_null() {
            return Err((INVALID, "null setter arguments"));
        }
        handle(p)?.set(tag, &*args)?;
        Ok(1)
    })
}
