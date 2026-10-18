//! libtiff's field registry: the schemas of each kind of directory, the
//! fields programs register, and anonymous fields for tags no schema names.
//! C sees a field through an opaque TIFFField pointer to its `View`.
use super::*;
use schema::kind;
use std::{ffi::CStr, io::Write};
use verified_tiff::adapter_meta as checked;

/// The kinds of directory libtiff reads, each with fields of its own.
pub const MAIN: u32 = 0;
pub const EXIF: u32 = 1;
pub const GPS: u32 = 2;
pub const CUSTOM: u32 = 3;

/// libtiff's value counts that depend on the value: any count, one value
/// per sample, and any count passed in 32 bits.
pub const VARIABLE: i32 = -1;
pub const SPP: i32 = -2;
pub const VARIABLE2: i32 = -3;

/// libtiff's `field_bit` of a field stored as a custom value.
const FIELD_CUSTOM: u16 = 65;
/// Registered fields a handle can hold, and the longest name they can have.
const MAX_FIELDS: usize = 4096;
const MAX_NAME: usize = 4095;

/// How a field's values are stored, set and got.
#[derive(Clone, Copy)]
pub struct Shape {
    pub tag: u16,
    /// The TIFF type the field is registered with, and the type its values
    /// are set and got in.
    pub kind: u16,
    pub host: u16,
    pub read_count: i32,
    pub write_count: i32,
    /// Nonzero when the caller passes a count with the values.
    pub pass_count: i32,
    /// Bytes in each value as set and got, or 0 for libtiff's conventions
    /// that are special to a tag.
    pub size: usize,
    /// Bytes in a passed count, or 0.
    pub count_size: usize,
    pub custom: bool,
    pub array: bool,
    pub anonymous: bool,
}

/// A row of the generated schemas.
pub struct Schema {
    pub shape: Shape,
    pub name: &'static CStr,
}

/// A schema row; schema.rs lists the arguments in order.
#[allow(clippy::too_many_arguments)]
pub const fn field(
    tag: u16,
    kind: u16,
    read_count: i32,
    write_count: i32,
    pass_count: bool,
    size: usize,
    count_size: usize,
    custom: bool,
    name: &'static CStr,
    host: u16,
    array: bool,
) -> Schema {
    let shape = Shape {
        tag,
        kind,
        host,
        read_count,
        write_count,
        pass_count: pass_count as i32,
        size,
        count_size,
        custom,
        array,
        anonymous: false,
    };
    Schema { shape, name }
}

/// The schemas of a kind of directory.
fn schemas(dir: u32) -> &'static [Schema] {
    match dir {
        MAIN => &schema::TIFF,
        EXIF => &schema::EXIF,
        GPS => &schema::GPS,
        _ => &[],
    }
}

/// The TIFFField layout that `bridge.h` declares for the C accessors.
#[repr(C)]
pub struct View {
    tag: u32,
    kind: i32,
    read_count: i32,
    write_count: i32,
    pass_count: i32,
    size: i32,
    count_size: i32,
    custom: i32,
    name: *const c_char,
    host: i32,
    array: i32,
    anonymous: i32,
}
const _: () = assert!(
    std::mem::size_of::<View>() == 56
        && std::mem::offset_of!(View, name) == 32
        && std::mem::offset_of!(View, host) == 40
        && std::mem::offset_of!(View, anonymous) == 48
);
impl View {
    fn new(s: &Shape, name: *const c_char) -> Self {
        Self {
            tag: s.tag.into(),
            kind: s.kind.into(),
            read_count: s.read_count,
            write_count: s.write_count,
            pass_count: s.pass_count,
            size: s.size as i32,
            count_size: s.count_size as i32,
            custom: s.custom.into(),
            name,
            host: s.host.into(),
            array: s.array.into(),
            anonymous: s.anonymous.into(),
        }
    }
}

/// A field C can see. Each lives in an allocation of its own, so the view C
/// holds stays where it is until the field is removed.
struct Entry {
    view: View,
    shape: Shape,
    dir: u32,
    /// The schema a field of the generated tables shows, or the name a
    /// registered field owns, with its NUL.
    schema: Option<&'static Schema>,
    _name: Option<Buf<u8>>,
}

enum Found {
    Registered(usize),
    Static(&'static Schema),
}

pub struct Fields {
    entries: Buf<Buf<Entry>>,
    /// Set when a registration fails, as a tag extender's may.
    pub failed: bool,
}
impl Fields {
    pub fn new(mem: &Mem) -> R<Self> {
        Ok(Self {
            entries: Buf::new(0, mem)?,
            failed: false,
        })
    }
    fn registered(&self) -> impl DoubleEndedIterator<Item = (usize, &Entry)> {
        self.entries
            .iter()
            .map(|e| &e[0])
            .enumerate()
            .filter(|(_, e)| e.schema.is_none())
    }
    /// Like libtiff, the newest registration of a tag comes first, then
    /// the schemas.
    fn locate(&self, dir: u32, tag: u32) -> Option<Found> {
        if let Some((i, _)) = self.registered().rev().find(|(_, e)| e.dir == dir && u32::from(e.shape.tag) == tag) {
            return Some(Found::Registered(i));
        }
        schemas(dir).iter().find(|s| u32::from(s.shape.tag) == tag).map(Found::Static)
    }
    fn locate_name(&self, dir: u32, name: &[u8]) -> Option<Found> {
        let named = |e: &&Entry| unsafe { CStr::from_ptr(e.view.name) }.to_bytes() == name;
        if let Some((i, _)) = self.registered().rev().find(|(_, e)| e.dir == dir && named(e)) {
            return Some(Found::Registered(i));
        }
        schemas(dir).iter().find(|s| s.name.to_bytes() == name).map(Found::Static)
    }
    /// The name of the field of `tag`.
    pub fn name(&self, dir: u32, tag: u32) -> Option<&CStr> {
        Some(match self.locate(dir, tag)? {
            // A registered field's view names the copy the entry owns.
            Found::Registered(i) => unsafe { CStr::from_ptr(self.entries[i][0].view.name) },
            Found::Static(s) => s.name,
        })
    }
    pub fn lookup(&self, dir: u32, tag: u32) -> Option<Shape> {
        match self.locate(dir, tag)? {
            Found::Registered(i) => Some(self.entries[i][0].shape),
            Found::Static(s) => Some(s.shape),
        }
    }
    fn push(&mut self, entry: Entry, mem: &Mem) -> R<()> {
        let mut one = Buf::new(1, mem)?;
        one.push(entry)?;
        self.entries.reserve((self.entries.len() + 1).next_power_of_two())?;
        self.entries.push(one)
    }
    /// The view of a field, made the first time C asks for a schema's.
    fn view(&mut self, dir: u32, found: Found, mem: &Mem) -> R<*const View> {
        let schema = match found {
            Found::Registered(i) => return Ok(&self.entries[i][0].view),
            Found::Static(s) => s,
        };
        let shown = |e: &&Buf<Entry>| e[0].schema.is_some_and(|s| ptr::eq(s, schema));
        if self.entries.iter().find(shown).is_none() {
            let view = View::new(&schema.shape, schema.name.as_ptr());
            let entry = Entry { view, shape: schema.shape, dir, schema: Some(schema), _name: None };
            self.push(entry, mem)?;
        }
        Ok(&self.entries.iter().find(shown).ok_or((5, "missing field view"))?[0].view)
    }
    /// Fail like libtiff's registration once a handle holds its limit.
    pub fn admit(&mut self) -> R<()> {
        if self.registered().count() >= MAX_FIELDS {
            self.failed = true;
            return Err((LIMIT, "field registration limit"));
        }
        Ok(())
    }
    pub fn register(&mut self, dir: u32, shape: Shape, name: &[u8], mem: &Mem) -> R<()> {
        let result = (|| {
            self.admit()?;
            if name.len() > MAX_NAME || name.contains(&0) {
                return Err((INVALID, "field name too long"));
            }
            let mut owned = Buf::new(name.len() + 1, mem)?;
            owned.extend(name)?;
            owned.push(0)?;
            let view = View::new(&shape, owned.as_ptr().cast());
            self.push(Entry { view, shape, dir, schema: None, _name: Some(owned) }, mem)
        })();
        if result.is_err() {
            self.failed = true;
        }
        result
    }
    /// Registrations from `mark` on are made, and later ones forgotten, with
    /// `rollback`.
    pub fn mark(&self) -> usize {
        self.entries.len()
    }
    pub fn rollback(&mut self, mark: usize) {
        let mut i = 0;
        self.entries.retain(|e| {
            i += 1;
            i <= mark || e[0].schema.is_some()
        });
    }
    /// Forget the custom directory fields registered before `mark`, which
    /// the fields of a newly read custom directory replace.
    pub fn replace_custom(&mut self, mark: usize) {
        let mut i = 0;
        self.entries.retain(|e| {
            i += 1;
            i > mark || e[0].dir != CUSTOM || e[0].schema.is_some()
        });
    }
}

/// libtiff's public TIFFFieldInfo.
#[repr(C)]
#[allow(dead_code)] // C's layout, of which some fields are unused.
struct FieldInfo {
    tag: u32,
    read_count: i16,
    write_count: i16,
    kind: i32,
    bit: u16,
    ok_to_change: u8,
    pass_count: u8,
    name: *const c_char,
}
/// libtiff 4.7.1's internal TIFFField and TIFFFieldArray, which programs
/// build to read a custom directory.
#[repr(C)]
#[allow(dead_code)]
struct ImportField {
    tag: u32,
    read_count: i16,
    write_count: i16,
    kind: i32,
    anonymous: u32,
    get_type: i32,
    bit: u16,
    ok_to_change: u8,
    pass_count: u8,
    name: *const c_char,
    children: *const c_void,
}
#[repr(C)]
#[allow(dead_code)]
struct ImportArray {
    kind: i32,
    allocated: u32,
    count: u32,
    fields: *const ImportField,
}
const _: () = assert!(
    std::mem::size_of::<FieldInfo>() == 24
        && std::mem::offset_of!(FieldInfo, name) == 16
        && std::mem::size_of::<ImportField>() == 40
        && std::mem::offset_of!(ImportField, name) == 24
        && std::mem::size_of::<ImportArray>() == 24
        && std::mem::offset_of!(ImportArray, fields) == 16
);

/// The bytes of a caller's string before its NUL, if it has one within
/// `MAX_NAME` bytes.
unsafe fn name<'a>(p: *const c_char) -> Option<&'a [u8]> {
    let p = p.cast::<u8>();
    (0..=MAX_NAME)
        .find(|&i| *p.add(i) == 0)
        .map(|n| std::slice::from_raw_parts(p, n))
}

/// The value type, count bytes and array flag of libtiff's TIFF_SETGET
/// codes for custom directory fields: a type without a count (1 to 13),
/// then the array types with no count, a 16-bit and a 32-bit count.
fn setget(code: i32) -> Option<(u16, usize, bool)> {
    const TYPES: [u16; 14] = [
        kind::NOTYPE, kind::ASCII, kind::BYTE, kind::SBYTE, kind::SHORT, kind::SSHORT, kind::LONG,
        kind::SLONG, kind::LONG8, kind::SLONG8, kind::FLOAT, kind::DOUBLE, kind::IFD8, kind::SLONG,
    ];
    let (base, count_size) = match code {
        1..=13 => (code, 0),
        15..=26 => (code - 14, 0),
        27..=38 => (code - 26, 2),
        39..=50 => (code - 38, 4),
        _ => return None,
    };
    let host = TYPES[base as usize];
    Some((host, count_size, code >= 15 || host == kind::ASCII))
}

impl Handle {
    /// The field of `tag` in the current directory. Like libtiff, a tag the
    /// directory holds that no schema or registration names gets an
    /// anonymous field.
    pub fn resolve(&mut self, tag: u32) -> R<Option<Shape>> {
        if let Some(s) = self.fields.lookup(self.directory_kind, tag) {
            return Ok(Some(s));
        }
        let Ok(key) = u16::try_from(tag) else { return Ok(None) };
        let Some(t) = find(&self.meta.tags, key).filter(|t| t.remote.is_none()) else {
            return Ok(None);
        };
        let host = checked::host_type(t.kind);
        let shape = Shape {
            tag: key,
            kind: t.kind,
            host,
            read_count: VARIABLE2,
            write_count: VARIABLE2,
            pass_count: 1,
            size: container::field_size(host),
            count_size: 4,
            custom: true,
            array: true,
            anonymous: true,
        };
        let mut text = [0u8; 16];
        let mut cursor = &mut text[..];
        write!(cursor, "Tag {tag}").map_err(|_| (5, "anonymous field name"))?;
        let n = 16 - cursor.len();
        self.fields.register(self.directory_kind, shape, &text[..n], &self.source.mem)?;
        Ok(Some(shape))
    }
    fn expose(&mut self, found: Option<Found>) -> R<*const View> {
        match found {
            Some(f) => self.fields.view(self.directory_kind, f, &self.source.mem),
            None => Ok(ptr::null()),
        }
    }
    /// TIFFMergeFieldInfo: register each field the directory kind does not
    /// have yet, or none.
    unsafe fn merge(&mut self, info: *const FieldInfo, n: u32) -> R<()> {
        let mark = self.fields.mark();
        let result = (|| {
            if (info.is_null() && n != 0) || n as usize > MAX_FIELDS {
                return Err((INVALID, "invalid field array"));
            }
            for i in 0..n as usize {
                let x = ptr::read_unaligned(info.add(i));
                if self.fields.lookup(self.directory_kind, x.tag).is_some() {
                    continue;
                }
                let kind = x.kind as u16;
                let host = checked::host_type(kind);
                let size = container::field_size(host);
                let (rc, wc, pass) = (x.read_count.into(), x.write_count.into(), x.pass_count.into());
                if x.name.is_null() || size == 0 || x.tag > 0xffff || !checked::registration(kind, rc, wc, pass != 0) {
                    return Err((INVALID, "invalid field definition"));
                }
                let shape = Shape {
                    tag: x.tag as u16,
                    kind,
                    host,
                    read_count: rc,
                    write_count: wc,
                    pass_count: pass,
                    size,
                    count_size: if pass == 0 { 0 } else if rc == VARIABLE2 { 4 } else { 2 },
                    custom: x.bit == FIELD_CUSTOM,
                    array: pass != 0 || rc != 1 || host == kind::ASCII,
                    anonymous: false,
                };
                self.fields.admit()?;
                let name = name(x.name).ok_or((INVALID, "field name too long"))?;
                self.fields.register(self.directory_kind, shape, name, &self.source.mem)?;
            }
            Ok(())
        })();
        if result.is_err() {
            self.fields.rollback(mark);
            self.fields.failed = true;
        }
        result
    }
    /// TIFFReadCustomDirectory: read the directory at `off` with the fields
    /// of `array`, which replace those of any earlier custom directory.
    unsafe fn read_custom(&mut self, off: u64, array: *const ImportArray) -> R<()> {
        let (count, fields) = if array.is_null() {
            (0, ptr::null())
        } else {
            let a = ptr::read_unaligned(array);
            (a.count as usize, a.fields)
        };
        if count > MAX_FIELDS || (count != 0 && fields.is_null()) {
            return Err((INVALID, "invalid custom field array"));
        }
        let mark = self.fields.mark();
        let result = (|| {
            for i in 0..count {
                let f = ptr::read_unaligned(fields.add(i));
                let Some((host, count_size, array)) = setget(f.get_type).filter(|_| !f.name.is_null() && f.tag <= 0xffff)
                else {
                    return Err((INVALID, "unsupported custom field convention"));
                };
                let shape = Shape {
                    tag: f.tag as u16,
                    kind: f.kind as u16,
                    host,
                    read_count: f.read_count.into(),
                    write_count: f.write_count.into(),
                    pass_count: f.pass_count.into(),
                    size: container::field_size(host),
                    count_size,
                    custom: true,
                    array,
                    anonymous: f.anonymous != 0,
                };
                self.fields.admit()?;
                let name = name(f.name).ok_or((INVALID, "field name too long"))?;
                self.fields.register(CUSTOM, shape, name, &self.source.mem)?;
            }
            self.custom_dir(off, CUSTOM)
        })();
        match result {
            Ok(()) => self.fields.replace_custom(mark),
            Err(_) => self.fields.rollback(mark),
        }
        result
    }
    /// The directory's tags with custom fields, as TIFFGetTagListEntry
    /// lists them. A tag whose anonymous field cannot be registered is left
    /// out.
    fn custom_tag(&mut self, mut index: usize) -> Option<u32> {
        for i in 0..self.meta.tags.len() {
            let tag = self.meta.tags[i].key.into();
            if self.resolve(tag).ok().flatten().is_some_and(|s| s.custom) {
                if index == 0 {
                    return Some(tag);
                }
                index -= 1;
            }
        }
        None
    }
}

#[no_mangle]
pub unsafe extern "C" fn vt_merge_fields(p: *mut c_void, info: *const c_void, n: u32) -> i32 {
    guard(0, || {
        handle(p)?.merge(info.cast(), n)?;
        Ok(1)
    })
}
#[no_mangle]
pub unsafe extern "C" fn vt_read_custom_directory(p: *mut c_void, off: u64, array: *const c_void) -> i32 {
    guard(0, || {
        handle(p)?.read_custom(off, array.cast())?;
        Ok(1)
    })
}
/// Before a tag extender runs, and whether a registration it made failed.
#[no_mangle]
pub unsafe extern "C" fn vt_fields_begin(p: *mut c_void) {
    guard((), || {
        handle(p)?.fields.failed = false;
        Ok(())
    })
}
#[no_mangle]
pub unsafe extern "C" fn vt_fields_failed(p: *mut c_void) -> i32 {
    guard(1, || Ok(handle(p)?.fields.failed.into()))
}
/// TIFFFindField: 1 with the field of `tag` of type `kind` (or any type),
/// 0 when there is none, -1 on failure. `resolve` gives a tag the directory
/// holds an anonymous field.
#[no_mangle]
pub unsafe extern "C" fn vt_find_field(p: *mut c_void, tag: u32, kind: i32, resolve: i32, out: *mut *const View) -> i32 {
    guard(-1, || {
        let h = handle(p)?;
        if out.is_null() {
            return Err((INVALID, "null field output"));
        }
        if resolve != 0 {
            h.resolve(tag)?;
        }
        let found = h.fields.locate(h.directory_kind, tag).filter(|f| {
            let s = match f {
                Found::Registered(i) => h.fields.entries[*i][0].shape,
                Found::Static(s) => s.shape,
            };
            kind == i32::from(kind::ANY) || kind == i32::from(s.kind)
        });
        *out = h.expose(found)?;
        Ok((!out.read().is_null()).into())
    })
}
#[no_mangle]
pub unsafe extern "C" fn vt_field_named(p: *mut c_void, text: *const c_char, out: *mut *const View) -> i32 {
    guard(-1, || {
        let h = handle(p)?;
        if out.is_null() || text.is_null() {
            return Err((INVALID, "null field name"));
        }
        let found = h.fields.locate_name(h.directory_kind, CStr::from_ptr(text).to_bytes());
        *out = h.expose(found)?;
        Ok((!out.read().is_null()).into())
    })
}
/// TIFFGetTagListCount and TIFFGetTagListEntry.
#[no_mangle]
pub unsafe extern "C" fn vt_custom_tags(p: *mut c_void) -> u32 {
    guard(0, || {
        let h = handle(p)?;
        let mut n = 0;
        while h.custom_tag(n).is_some() {
            n += 1;
        }
        Ok(n as u32)
    })
}
#[no_mangle]
pub unsafe extern "C" fn vt_custom_tag(p: *mut c_void, index: u32) -> u32 {
    guard(u32::MAX, || Ok(handle(p)?.custom_tag(index as usize).unwrap_or(u32::MAX)))
}
/// The next tag from `*at` on that has no field, as TIFFReadDirectory warns
/// about when the open options ask it to, or `u32::MAX`.
#[no_mangle]
pub unsafe extern "C" fn vt_unknown_tag(p: *mut c_void, at: *mut usize) -> u32 {
    guard(u32::MAX, || {
        let h = handle(p)?;
        if at.is_null() {
            return Err((INVALID, "null tag cursor"));
        }
        while h.warn_unknown && *at < h.meta.tags.len() {
            let tag = h.meta.tags[*at].key.into();
            *at += 1;
            if h.fields.lookup(h.directory_kind, tag).is_none() {
                return Ok(tag);
            }
        }
        Ok(u32::MAX)
    })
}
