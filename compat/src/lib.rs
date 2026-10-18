//! Unverified ABI adapter. The verified decoder remains a separate dependency.
//! All foreign pointers must obey the C API's validity, ownership and alias rules.
use std::{
    cell::RefCell,
    ffi::{c_char, c_void},
    os::fd::FromRawFd,
    panic::{catch_unwind, AssertUnwindSafe},
    ptr,
};
use verified_tiff::{
    decode_jpeg_into, inspect_raster, read32, Budget, Error, Limits,
};
type R<T> = Result<T, (i32, &'static str)>;
const INVALID: i32 = 22;
const NOTIMPL: i32 = 38;
const LIMIT: i32 = 27;
const NOMEM: i32 = 12;
const IO: i32 = 5;
fn err(e: Error) -> (i32, &'static str) {
    match e {
        Error::Unsupported => (NOTIMPL, "TIFF format not implemented by verified-tiff"),
        Error::Limit => (LIMIT, "verified-tiff resource limit"),
        Error::Truncated => (INVALID, "truncated TIFF input"),
        Error::Invalid => (INVALID, "invalid TIFF input"),
        Error::PageNotFound => (INVALID, "directory not found"),
        Error::OutputTooSmall => (INVALID, "output buffer too small"),
    }
}
thread_local! {static ERROR:RefCell<(i32,[u8;256])>=const{RefCell::new((0,[0;256]))};}
fn report(e: (i32, &str)) {
    ERROR.with(|a| {
        let mut a = a.borrow_mut();
        a.0 = e.0;
        a.1.fill(0);
        let n = e.1.len().min(255);
        a.1[..n].copy_from_slice(&e.1.as_bytes()[..n]);
    });
}
fn guard<T: Copy>(fallback: T, f: impl FnOnce() -> R<T>) -> T {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(v)) => v,
        Ok(Err(e)) => {
            report(e);
            fallback
        }
        Err(_) => {
            report((5, "internal adapter panic"));
            fallback
        }
    }
}
#[no_mangle]
pub extern "C" fn vt_error_code() -> i32 {
    ERROR.with(|a| a.borrow().0)
}
#[no_mangle]
pub extern "C" fn vt_error_message() -> *const c_char {
    ERROR.with(|a| a.borrow().1.as_ptr().cast())
}
fn product(a: usize, b: usize) -> R<usize> {
    verified_tiff::mul(a,b).map_err(err)
}
mod storage;
mod stream;
mod tag;
mod directory;
mod decoder;
mod lerc;
mod webp;
mod color;
mod metadata;
mod writer;
mod store;
mod index;
mod fields;
mod schema;
mod get;
mod set;
mod open;
mod read;
mod utility;
mod print;
use storage::{Buf,Mem,Memory,Source,Backend,ReadProc,SeekProc};
use directory::*;
use store::{Store,Values};
use verified_tiff::container;
/// A field's type and count, and where its value is: at `at` in slot `slot`
/// of the directory's store, or for a deferred strile array at `remote` in
/// the file.
#[derive(Clone,Copy)]
struct Tag{key:u16,kind:u16,count:usize,at:usize,slot:usize,remote:Option<u64>}
struct Meta{
    tags:Buf<Tag>,off:u64,next:u64,le:bool,width:usize,height:usize,ch:usize,bits:usize,
    format:u16,photo:u64,planar:usize,rows:usize,bw:usize,bh:usize,tiled:bool,
    // libtiff's tile length for strip images, and whether RowsPerStrip is set.
    tile_rows:usize,rows_set:bool,
    offsets:Striles,counts:Striles,map:Buf<u16>,extra:Buf<u16>,subifds:Buf<u64>,
    compression:u16,predictor:u16,orientation:usize,fill:u16,
}
struct Handle{
    source:Source,header:container::Header,data:Store,meta:Meta,page:u32,limits:Limits,
    work:usize,chain_steps:usize,chain:Buf<u64>,stream:Option<stream::Cached>,cache:Option<(usize,Buf<u8>)>,rendered:Option<Buf<u8>>,jpeg_rgb:bool,logluv16:bool,chop:bool,deferred:bool,converted:Buf<metadata::Converted>,directory_kind:u32,writing:bool,
    /// libtiff's current row, strip and tile.
    position:[u32;3],
    fields:fields::Fields,
    /// The ReferenceBlackWhite default TIFFGetFieldDefaulted last gave.
    default_reference:[f32;6],
    /// Whether directory reads warn of tags no field describes.
    warn_unknown:bool,
    /// Whether a writer's directory is written as it now stands.
    directory_written:bool,
    client_info:Buf<utility::ClientInfo>,
}
impl Handle{
    fn open(mut source:Source,options:&open::Options,flags:u32)->R<Self>{
        use open::{BIGENDIAN,BIGTIFF,CHOP,DEFERRED,HEADER,WRITE};
        if source.len>source.input as u64||source.len>isize::MAX as u64{return Err((LIMIT,"input byte limit"));}
        let writing=flags&WRITE!=0;
        let header=if writing{container::Header{little_endian:flags&BIGENDIAN==0,big:flags&BIGTIFF!=0,first_ifd:0}}else{let b=source.read(0,source.len.min(16) as usize)?;container::header(&b).map_err(err)?};
        let limits=Limits{max_input:source.input,max_output:options.output,..Limits::conservative()};
        let (work,warn_unknown)=(options.work,options.warn_unknown!=0);
        let chop=flags&CHOP!=0;let deferred=flags&DEFERRED!=0;
        let (data,meta)=if writing||flags&HEADER!=0{empty_metadata(&source.mem,header)?}else{metadata(&mut source,header,header.first_ifd,&limits,chop,deferred)?};
        let converted=Buf::new(0,&source.mem)?;let data=Store::new(data,&source.mem)?;let fields=fields::Fields::new(&source.mem)?;let client_info=Buf::new(0,&source.mem)?;
        let mut chain=Buf::new(1,&source.mem)?;if meta.off!=0{chain.push(meta.off)?;}
        // Like libtiff, a header-only open has no current directory yet.
        let page=if !writing&&flags&HEADER!=0{u32::MAX}else{0};
        Ok(Self{writing,warn_unknown,directory_written:false,client_info,converted,directory_kind:0,default_reference:[0.;6],position:[u32::MAX;3],fields,source,header,data,meta,page,limits,work,chain_steps:0,chain,stream:None,cache:None,rendered:None,jpeg_rgb:false,logluv16:false,chop,deferred})
    }
    fn set_offset(&mut self,off:u64,page:u32)->R<()>{
        let (data,meta)=metadata(&mut self.source,self.header,off,&self.limits,self.chop,self.deferred)?;
        let data=Store::new(data,&self.source.mem)?;
        self.converted=Buf::new(0,&self.source.mem)?;self.directory_kind=0;self.data=data;self.meta=meta;self.page=page;self.chain_steps=0;self.stream=None;self.cache=None;self.rendered=None;self.jpeg_rgb=false;self.logluv16=false;self.position=[u32::MAX;3];Ok(())
    }
    fn source_channels(&self)->R<usize>{Ok(self.meta.ch)}
    /// JPEGCOLORMODE or SGILOGDATAFMT: decode YCbCr JPEG as RGB, or LogLuv
    /// as 16-bit integers.
    fn mode(&mut self,key:u32,v:i32)->R<()>{
        self.stream=None;self.cache=None;
        match key {
            tag::JPEGCOLORMODE if self.meta.photo == 6 && (v == 0 || v == 1) => self.jpeg_rgb = v == 1,
            tag::SGILOGDATAFMT if self.meta.photo == 32845 && (v==0||v==1) => self.logluv16 = v==1,
            _ => return Err((NOTIMPL, "TIFFSetField mutation not implemented")),
        }
        Ok(())
    }
    fn raw_row(&self,w:usize)->R<usize>{
        if self.ycbcr_raw(){let (hs,vs)=self.sampling()?;return Ok(product((w-1)/hs+1,hs*vs+2)?/vs);}
        if self.meta.photo==6 && matches!(self.meta.compression,6|7) && !self.jpeg_rgb{return Err((NOTIMPL,"native subsampled YCbCr not implemented; request JPEGCOLORMODE_RGB"));}
        self.sample_row(w)
    }
    /// A row of whole samples, as libtiff's tile row and raster scanline
    /// sizes count it, without YCbCr subsampling.
    fn sample_row(&self,w:usize)->R<usize>{
        let row=self.meta.row_bytes(w)?;
        if self.meta.compression==34676 && self.meta.photo==32845 && !self.logluv16{product(row,2)}else{Ok(row)}
    }
}
fn empty_metadata(mem:&Mem,header:container::Header)->R<(Buf<u8>,Meta)>{
    Ok((Buf::new(0,mem)?,Meta{tags:Buf::new(0,mem)?,off:0,next:header.first_ifd,le:header.little_endian,width:0,height:0,ch:1,bits:1,format:1,photo:1,planar:1,rows:0,bw:0,bh:0,tile_rows:0,rows_set:false,tiled:false,offsets:Striles::empty(mem)?,counts:Striles::empty(mem)?,map:Buf::new(0,mem)?,extra:Buf::new(0,mem)?,subifds:Buf::new(0,mem)?,compression:1,predictor:1,orientation:1,fill:1}))
}
// Safety: only the C shim can call these hidden exports. A live handle must
// be exclusively accessed for the duration of a call; C owns its lifetime.
unsafe fn handle<'a>(p: *mut c_void) -> R<&'a mut Handle> {
    if p.is_null() {
        Err((INVALID, "null TIFF handle"))
    } else {
        Ok(&mut *p.cast::<Handle>())
    }
}
#[no_mangle]
pub unsafe extern "C" fn vt_open_fd(fd:i32,options:*const open::Options,base:usize,flags:u32)->*mut c_void{
    guard(ptr::null_mut(),||{
        let o=options.as_ref().ok_or((INVALID,"null options"))?;
        if fd<0{return Err((INVALID,"invalid file descriptor"));}
        let file=std::mem::ManuallyDrop::new(std::fs::File::from_raw_fd(fd));
        let len=file.metadata().map_err(|_|(5,"input stat failed"))?.len();
        let mem=Memory::new(o.single,o.cumulative,base.checked_add(std::mem::size_of::<Handle>()).ok_or((LIMIT,"handle size overflow"))?)?;
        let source=Source{read_buffer:None,backend:Backend::Fd(fd),len,mem,input:o.input};
        Ok(Box::into_raw(Box::new(Handle::open(source,o,flags)?)).cast())
    })
}
#[no_mangle]
pub unsafe extern "C" fn vt_open_client(client:*mut c_void,read:ReadProc,write:Option<ReadProc>,seek:SeekProc,len:u64,options:*const open::Options,base:usize,flags:u32)->*mut c_void{
    guard(ptr::null_mut(),||{
        let o=options.as_ref().ok_or((INVALID,"null options"))?;
        let mem=Memory::new(o.single,o.cumulative,base.checked_add(std::mem::size_of::<Handle>()).ok_or((LIMIT,"handle size overflow"))?)?;
        let source=Source{read_buffer:None,backend:Backend::Client{client,read,write,seek},len,mem,input:o.input};
        Ok(Box::into_raw(Box::new(Handle::open(source,o,flags)?)).cast())
    })
}
#[no_mangle]
pub unsafe extern "C" fn vt_close(h: *mut c_void) {
    guard((), || {
        if !h.is_null() {
            drop(Box::from_raw(h.cast::<Handle>()));
        }
        Ok(())
    });
}
#[no_mangle]
pub unsafe extern "C" fn vt_select(p: *mut c_void, page: u32) -> i32 {
    guard(0, || {
        let h = handle(p)?;
        let seen=page_offset(&mut h.source,h.header,Some(page),&h.limits)?;
        let mut chain=Buf::new(seen.len(),&h.source.mem)?;chain.extend(&seen)?;drop(seen);
        h.set_offset(chain[chain.len()-1],page)?;h.chain=chain;
        Ok(1)
    })
}
#[no_mangle]
pub unsafe extern "C" fn vt_info(p: *mut c_void, key: u32) -> u64 {
    guard(u64::MAX, || {
        let h = handle(p)?;
        let m = &h.meta;
        Ok(match key {
            0 => m.width as u64,
            1 => m.height as u64,
            2 => h.page as u64,
            3 => m.off as u64,
            4 => m.next as u64,
            5 => m.tiled as u64,
            6 => m.le as u64,
            7 => m.offsets.len() as u64,
            8 => m.bw as u64,
            9 => m.bh as u64,
            10 => m.rows as u64,
            11 => m.planar as u64,
            12 => h.source_channels()? as u64,
            13 => h.limits.max_output as u64,
            14 => h.header.big as u64,
            15 => h.meta.tags.len() as u64,
            16 => m.photo,
            _ => return Err((INVALID, "unknown adapter info")),
        })
    })
}
#[no_mangle]
pub unsafe extern "C" fn vt_count_dirs(p: *mut c_void) -> u32 {
    guard(0, || {
        let h = handle(p)?;
        Ok(page_offset(&mut h.source,h.header,None,&h.limits)?.len() as u32)
    })
}
#[no_mangle]
pub unsafe extern "C" fn vt_set_offset(p: *mut c_void, offset: u64) -> i32 {
    guard(0, || {
        let h = handle(p)?;
        // Like libtiff, a main-chain directory keeps its number and loop
        // history; any other directory starts a new chain as number 0.
        let seen=page_offset(&mut h.source,h.header,None,&h.limits).ok();
        let at=seen.as_ref().and_then(|s|s.iter().position(|o|*o==offset));
        let mut chain=Buf::new(at.map_or(1,|i|i+1),&h.source.mem)?;
        match (&seen,at){(Some(s),Some(i))=>chain.extend(&s[..=i])?,_=>chain.push(offset)?}
        h.set_offset(offset,at.map_or(0,|i|i as u32))?;h.chain=chain;
        Ok(1)
    })
}
#[no_mangle]
pub unsafe extern "C" fn vt_size(p: *mut c_void, kind: u32, rows: u32) -> i64 {
    guard(-1, || {
        let h = handle(p)?;
        let m = &h.meta;
        if h.meta.width==0 {return Err((INVALID,"no selected image directory"));}
        if kind > 4 {
            return Err((INVALID, "invalid size operation"));
        }
        // Like libtiff, a strip image's tiles are its strips: image width by
        // RowsPerStrip rows.
        let w = if kind == 2 || kind == 3 {
            m.bw
        } else {
            m.width
        };
        let n = if kind == 0 || kind == 2 || kind == 4 {
            1
        } else if rows == u32::MAX {
            // Like libtiff, a tiled image's strip spans RowsPerStrip rows.
            if kind == 1 && m.tiled { m.rows.min(m.height) } else if kind == 3 && !m.tiled { m.tile_rows } else { m.bh }
        } else {
            rows as usize
        };
        let size=match kind{
            1|3=>if n==0{0}else{let (row,rows)=h.native_layout(w,n)?;product(row,rows)?},
            2=>h.sample_row(w)?,
            4=>product(h.sample_row(w)?,if m.planar==2{m.ch}else{1})?,
            _=>h.raw_row(w)?,
        };
        if size > i64::MAX as usize {
            return Err((LIMIT, "size overflow"));
        }
        Ok(size as i64)
    })
}
#[no_mangle]
pub unsafe extern "C" fn vt_rgba_ok(p: *mut c_void) -> i32 {
    guard(0, || {
        let h = handle(p)?;
        h.rgba_ok()?;
        Ok(1)
    })
}
#[no_mangle]
pub unsafe extern "C" fn vt_next(p:*mut c_void)->i32{
    guard(0,||{let h=handle(p)?;let off=h.meta.next;if off==0{return Ok(0);}
        // Like libtiff, stop at any directory this chain already visited.
        if off==h.meta.off||h.chain.contains(&off){return Err((40,"cyclic directory chain"));}
        let steps=verified_tiff::adapter_bounds::directory_step(h.chain_steps,h.limits.max_directories).map_err(err)?;
        h.chain.reserve((h.chain.len()+1).next_power_of_two())?;
        let page=if h.meta.off==0{0}else{h.page.saturating_add(1)};h.set_offset(off,page)?;h.chain_steps=steps;h.chain.push(off)?;Ok(1)})
}
#[no_mangle]
pub unsafe extern "C" fn vt_strile(p:*mut c_void,k:u32,counts:i32,out:*mut u64)->i32{
    guard(0,||{let h=handle(p)?;if out.is_null(){return Err((INVALID,"null strile output"));}
        *out=if counts!=0{h.meta.counts.get(&mut h.source,k as usize)?}else{h.meta.offsets.get(&mut h.source,k as usize)?};Ok(1)})
}

#[no_mangle]
pub unsafe extern "C" fn vt_unset(p:*mut c_void,key:u32)->i32{
    guard(0,||{let h=handle(p)?;
        if h.directory_kind==0&&key==u32::from(tag::NUMBEROFINKS)&&find(&h.meta.tags,tag::INKNAMES).is_some(){return Err((INVALID,"NumberOfInks is required by InkNames"));}
        let mut tags=Buf::new(h.meta.tags.len(),&h.source.mem)?;
        for t in h.meta.tags.iter(){if t.key as u32!=key{tags.push(*t)?;}}
        // Like libtiff, only the unset field's storage is freed.
        h.data.release(&tags);h.meta.tags=tags;h.converted.retain(|c|c.key!=key);h.stream=None;h.cache=None;h.rendered=None;Ok(1)})
}
#[no_mangle]
pub unsafe extern "C" fn vt_free_directory(p:*mut c_void)->i32{
    guard(0,||{let h=handle(p)?;let (data,mut meta)=empty_metadata(&h.source.mem,h.header)?;
        meta.off=h.meta.off;meta.next=h.meta.next;let data=Store::new(data,&h.source.mem)?;
        h.meta=meta;h.data=data;h.converted=Buf::new(0,&h.source.mem)?;h.stream=None;h.cache=None;h.rendered=None;h.position=[u32::MAX;3];Ok(1)})
}

#[no_mangle]
pub unsafe extern "C" fn vt_clientdata(p:*mut c_void,client:*mut c_void)->i32{
    guard(0,||{let h=handle(p)?;if let Backend::Client{client:current,..}=&mut h.source.backend{*current=client;}
        h.stream=None;h.cache=None;h.rendered=None;Ok(1)})
}
