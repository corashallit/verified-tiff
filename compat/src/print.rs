//! TIFFPrintDirectory's text: the directory's offset, then each field it
//! holds with up to 32 of its values, and with TIFFPRINT_STRIPS the strile
//! arrays. The format is the adapter's own, not libtiff's.
use super::*;
use schema::kind;
use std::fmt::{self, Display, LowerExp, Write};
use verified_tiff::{adapter_meta as checked, native_samples::read_word};

/// TIFFPRINT_STRIPS.
const PRINT_STRIPS: i64 = 1;
/// The values shown of a field, before a count of them all.
const SHOWN: usize = 32;
/// Reals smaller or larger than these print with an exponent.
const SMALL: f64 = 1e-4;
const LARGE: f64 = 1e16;

/// Text that grows within the handle's allocation and output limits.
struct Text {
    buf: Buf<u8>,
    limit: usize,
    error: Option<(i32, &'static str)>,
}
impl Text {
    fn bytes(&mut self, s: &[u8]) -> R<()> {
        let n = self.buf.len().checked_add(s.len()).filter(|&n| n <= self.limit);
        let n = n.ok_or((LIMIT, "directory text limit"))?;
        self.buf.reserve(n.checked_next_power_of_two().unwrap_or(n).min(self.limit))?;
        self.buf.extend(s)
    }
    fn print(&mut self, args: fmt::Arguments) -> R<()> {
        self.write_fmt(args)
            .map_err(|_| self.error.take().unwrap_or((5, "directory text")))
    }
    fn real<T: Display + LowerExp>(&mut self, v: T, magnitude: f64) -> R<()> {
        if magnitude != 0. && magnitude.is_finite() && !(SMALL..LARGE).contains(&magnitude) {
            self.print(format_args!(" {v:e}"))
        } else {
            self.print(format_args!(" {v}"))
        }
    }
}
impl Write for Text {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        self.bytes(s.as_bytes()).map_err(|e| {
            self.error = Some(e);
            fmt::Error
        })
    }
}

impl Handle {
    fn print_directory(&mut self, flags: i64) -> R<Buf<u8>> {
        let mut f = Text {
            buf: Buf::new(0, &self.source.mem)?,
            limit: self.limits.max_output,
            error: None,
        };
        let off = self.meta.off;
        f.print(format_args!("TIFF Directory at offset {off:#x} ({off})\n"))?;
        let le = self.meta.le;
        for i in 0..self.meta.tags.len() {
            let t = self.meta.tags[i];
            let key = u32::from(t.key);
            // Like libtiff, a tag no field describes gets an anonymous one.
            self.resolve(key)?;
            let Some(name) = self.fields.name(self.directory_kind, key) else {
                continue;
            };
            if matches!(
                t.key,
                tag::STRIPOFFSETS | tag::STRIPBYTECOUNTS | tag::TILEOFFSETS | tag::TILEBYTECOUNTS
            ) {
                if flags & PRINT_STRIPS == 0 {
                    continue;
                }
                f.print(format_args!("  {}:", name.to_string_lossy()))?;
                let counts = matches!(t.key, tag::STRIPBYTECOUNTS | tag::TILEBYTECOUNTS);
                let m = &mut self.meta;
                for k in 0..m.offsets.len() {
                    let striles = if counts { &mut m.counts } else { &mut m.offsets };
                    let v = striles.get(&mut self.source, k)?;
                    f.print(format_args!(" {v}"))?;
                }
                f.bytes(b"\n")?;
                continue;
            }
            let s = self.data.bytes(&t);
            let Ok(len) = checked::payload(s.len(), t.at, t.count, t.kind) else {
                continue;
            };
            f.print(format_args!("  {} ({key}):", name.to_string_lossy()))?;
            if t.kind == kind::ASCII {
                f.bytes(b" ")?;
                for &c in s[t.at..t.at + len].iter().take_while(|&&c| c != 0) {
                    if (32..127).contains(&c) {
                        f.bytes(&[c])?;
                    } else {
                        f.print(format_args!("\\x{c:02x}"))?;
                    }
                }
            } else {
                let w = container::field_size(t.kind);
                for j in 0..t.count.min(SHOWN) {
                    let at = t.at + j * w;
                    match t.kind {
                        kind::FLOAT => {
                            let v = f32::from_bits(read_word(s, at, w, le).map_err(err)? as u32);
                            f.real(v, v.abs().into())?;
                        }
                        kind::DOUBLE | kind::RATIONAL | kind::SRATIONAL => {
                            let v = metadata::real(s, at, t.kind, le, false)?;
                            f.real(v, v.abs())?;
                        }
                        kind::SBYTE | kind::SSHORT | kind::SLONG | kind::SLONG8 => {
                            let v = metadata::signed(read_word(s, at, w, le).map_err(err)?, w);
                            f.print(format_args!(" {v}"))?;
                        }
                        _ => {
                            let v = read_word(s, at, w, le).map_err(err)?;
                            f.print(format_args!(" {v}"))?;
                        }
                    }
                }
                if t.count > SHOWN {
                    f.print(format_args!(" ... ({} values)", t.count))?;
                }
            }
            f.bytes(b"\n")?;
        }
        Ok(f.buf)
    }
}

/// TIFFPrintDirectory's text, which lasts until the handle's next read or
/// change.
#[no_mangle]
pub unsafe extern "C" fn vt_print_directory(p: *mut c_void, flags: i64, n: *mut usize) -> *const u8 {
    guard(ptr::null(), || {
        if n.is_null() {
            return Err((INVALID, "null text length"));
        }
        let h = handle(p)?;
        let text = h.print_directory(flags)?;
        let text = h.rendered.insert(text);
        *n = text.len();
        Ok(text.as_ptr())
    })
}
