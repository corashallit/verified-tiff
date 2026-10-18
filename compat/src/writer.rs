//! Metadata-only temporary TIFF construction for read-side codec clients.
//! This does not implement image encoding or modify an opened reader source.
use super::*;
fn append_number(b:&mut Buf<u8>,v:u64,w:usize,le:bool)->R<()>{let a=if le{v.to_le_bytes()}else{v.to_be_bytes()};b.extend(if le{&a[..w]}else{&a[8-w..]})}
impl Handle{
    pub fn write_check(&mut self)->R<()>{
        if !self.writing{return Err((NOTIMPL,"reader is not writable"));}
        let le=self.header.little_endian;
        let sc=|key,d|scalar(&self.data,&self.meta.tags,key,d,le);
        let width=sc(tag::IMAGEWIDTH,0)?;let height=sc(tag::IMAGELENGTH,0)?;let ch=sc(tag::SAMPLESPERPIXEL,1)?;let planar=sc(tag::PLANARCONFIG,1)?;
        if width==0||height==0||width>u32::MAX as u64||height>u32::MAX as u64||ch==0||ch>64||!matches!(planar,1|2){return Err((INVALID,"invalid temporary image geometry"));}
        let tiled=find(&self.meta.tags,tag::TILEWIDTH).is_some()||find(&self.meta.tags,tag::TILELENGTH).is_some();
        let bw=if tiled{sc(tag::TILEWIDTH,0)?}else{width};let bh=if tiled{sc(tag::TILELENGTH,0)?}else{sc(tag::ROWSPERSTRIP,height)?.min(height)};
        if bw==0||bh==0||bw>u32::MAX as u64||bh>u32::MAX as u64{return Err((INVALID,"invalid temporary block geometry"));}
        let count=product(product(((width-1)/bw+1) as usize,((height-1)/bh+1) as usize)?,if planar==2{ch as usize}else{1})?;
        if count>self.limits.max_segments{return Err((LIMIT,"temporary segment count"));}
        let kind=if self.header.big{16}else{4};let bytes=product(count,container::field_size(kind))?;
        let zeros=Buf::zeros(bytes,self.limits.max_output,&self.source.mem)?;
        for key in if tiled{[tag::TILEOFFSETS,tag::TILEBYTECOUNTS]}else{[tag::STRIPOFFSETS,tag::STRIPBYTECOUNTS]}{if find(&self.meta.tags,key).is_none(){self.put(key,kind,count,&zeros)?;}}
        // Validate assembled geometry using an independent copy; tag pointers
        // stay valid for every value it leaves unchanged.
        let (data,tags)=self.data.arena(&self.meta.tags,None,0,&self.source.mem)?;
        let (data,meta)=assemble(data,tags,0,0,le,&self.limits,false,&self.source.mem,false,None)?;
        self.commit(&data,meta)
    }
    fn directory_bytes(&mut self)->R<()>{
        self.write_check()?;
        let big=self.header.big;let le=self.header.little_endian;
        let hs=if big{16}else{8};let cw=if big{8}else{2};let ew=if big{20}else{12};let slot=if big{8}else{4};
        let table=product(self.meta.tags.len(),ew)?.checked_add(hs+cw+slot).ok_or((LIMIT,"directory size"))?;
        let mut total=table;
        for t in self.meta.tags.iter(){let n=product(t.count,container::field_size(t.kind))?;if n>slot{total=total.checked_add(n).and_then(|n|n.checked_add(7)).ok_or((LIMIT,"directory size"))?;}}
        if !big&&total>u32::MAX as usize{return Err((LIMIT,"Classic TIFF size"));}
        let mut out=Buf::new(total,&self.source.mem)?;
        out.extend(if le{b"II"}else{b"MM"})?;append_number(&mut out,if big{43}else{42},2,le)?;
        if big{append_number(&mut out,8,2,le)?;append_number(&mut out,0,2,le)?;}
        append_number(&mut out,hs as u64,slot,le)?;append_number(&mut out,self.meta.tags.len() as u64,cw,le)?;
        let mut offset=table;
        for t in self.meta.tags.iter(){
            let n=product(t.count,container::field_size(t.kind))?;
            append_number(&mut out,t.key as u64,2,le)?;append_number(&mut out,t.kind as u64,2,le)?;append_number(&mut out,t.count as u64,slot,le)?;
            if n<=slot{out.extend(&self.data.bytes(t)[t.at..t.at+n])?;for _ in n..slot{out.push(0)?;}}
            else{offset=offset.checked_add(7).ok_or((LIMIT,"directory size"))?&!7;append_number(&mut out,offset as u64,slot,le)?;offset+=n;}
        }
        append_number(&mut out,0,slot,le)?;
        for t in self.meta.tags.iter(){let n=product(t.count,container::field_size(t.kind))?;if n>slot{while out.len()%8!=0{out.push(0)?;}out.extend(&self.data.bytes(t)[t.at..t.at+n])?;}}
        if out.len()>self.limits.max_input{return Err((LIMIT,"temporary file limit"));}
        self.rendered=Some(out);Ok(())
    }
}
/// libtiff's write callbacks are passed at most this many bytes at once.
const CHUNK:usize=1<<16;
const SEEK_SET:i32=0;
impl Handle{
    /// Writes the directory over the client's output from its start.
    fn write_directory(&mut self)->R<()>{
        let Backend::Client{client,write:Some(write),seek,..}=self.source.backend else{return Err((NOTIMPL,"not implemented by verified-tiff"));};
        if !self.writing{return Err((NOTIMPL,"not implemented by verified-tiff"));}
        self.directory_bytes()?;
        let mut bytes=self.rendered.take().ok_or((IO,"missing directory bytes"))?;
        // Safety: the callbacks are the caller's, with the client they gave.
        if unsafe{seek(client,0,SEEK_SET)}!=0{return Err((IO,"temporary directory seek failed"));}
        let mut at=0;
        while at<bytes.len(){
            let want=(bytes.len()-at).min(CHUNK);
            let got=unsafe{write(client,bytes.as_mut_ptr().add(at).cast(),want as isize)};
            if got<=0||got as usize>want{return Err((IO,"temporary directory write failed"));}
            at+=got as usize;
        }
        self.directory_written=true;Ok(())
    }
}
/// TIFFWriteCheck: whether the directory can be written with strips, or
/// with `tiles`.
#[no_mangle]
pub unsafe extern "C" fn vt_write_check(p:*mut c_void,tiles:i32)->i32{
    guard(0,||{let h=handle(p)?;h.write_check()?;if (tiles!=0)!=h.meta.tiled{return Err((INVALID,"strip/tile layout mismatch"));}Ok(1)})
}
#[no_mangle]
pub unsafe extern "C" fn vt_write_directory(p:*mut c_void)->i32{guard(0,||{handle(p)?.write_directory()?;Ok(1)})}
/// TIFFFlush and TIFFClose: a writer writes its directory unless it is
/// already written.
#[no_mangle]
pub unsafe extern "C" fn vt_flush(p:*mut c_void)->i32{
    guard(0,||{let h=handle(p)?;if h.writing&&!h.directory_written{h.write_directory()?;}Ok(1)})
}
