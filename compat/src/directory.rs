//! Seekable directory assembly. Record validation is verified; allocation,
//! tag policy, chain traversal and geometry assembly belong to the C boundary.
use super::*;
use verified_tiff::{adapter_geometry as geometry,adapter_meta as checked};
use verified_tiff::{container as ct,native_samples::read_word};

pub fn u32at(s:&[u8],p:usize,le:bool)->R<u32>{read32(s,p,le).map_err(err)}
pub fn find(tags:&[Tag],key:u16)->Option<Tag>{tags.iter().find(|t|t.key==key).copied()}
pub fn value(s:&[u8],t:Tag,i:usize,le:bool)->R<u64>{
    if t.remote.is_some(){return Err((INVALID,"deferred field requires strile accessor"));}
    if i>=t.count{return Err((INVALID,"field index out of range"));}
    let w=match t.kind{1=>1,3=>2,4|13=>4,16|18=>8,_=>return Err((NOTIMPL,"integer field type not implemented"))};
    checked::payload(s.len(),t.at,t.count,t.kind).map_err(err)?;
    read_word(s,t.at+product(i,w)?,w,le).map_err(err)
}
pub fn scalar(s:&[u8],tags:&[Tag],key:u16,default:u64,le:bool)->R<u64>{match find(tags,key){Some(t)=>value(s,t,0,le),None=>Ok(default)}}
pub fn array(s:&[u8],tags:&[Tag],key:u16,le:bool,cap:usize,mem:&Mem)->R<Buf<u64>>{
    let t=find(tags,key);let n=t.map_or(0,|t|t.count);if n>cap{return Err((LIMIT,"metadata array limit"));}
    let mut a=Buf::new(n,mem)?;if let Some(t)=t{for i in 0..n{a.push(value(s,t,i,le)?)?;}}Ok(a)
}
fn small_array(s:&[u8],tags:&[Tag],key:u16,le:bool,cap:usize,mem:&Mem)->R<Buf<u16>>{
    let t=find(tags,key);let n=t.map_or(0,|t|t.count);if n>cap{return Err((LIMIT,"metadata array limit"));}
    let mut a=Buf::new(n,mem)?;if let Some(t)=t{for i in 0..n{a.push(u16::try_from(value(s,t,i,le)?).map_err(|_|(INVALID,"16-bit metadata overflow"))?)?;}}Ok(a)
}
fn add64(a:u64,b:u64)->R<u64>{a.checked_add(b).ok_or((LIMIT,"file offset overflow"))}
pub fn table(source:&mut Source,header:ct::Header,off:u64,l:&Limits)->R<(Buf<u8>,usize,u64)>{
    if off==0{return Err((INVALID,"directory not found"));}
    let cw=if header.big{8}else{2};let ew=if header.big{20}else{12};let nw=if header.big{8}else{4};
    let b=source.read(off,cw)?;let n=read_word(&b,0,cw,header.little_endian).map_err(err)?;
    if n>l.max_tags as u64{return Err((LIMIT,"directory tag limit"));}
    let n=n as usize;let bytes=product(n,ew)?.checked_add(nw).ok_or((LIMIT,"directory overflow"))?;
    let b=source.read(add64(off,cw as u64)?,bytes)?;
    let next=read_word(&b,n*ew,nw,header.little_endian).map_err(err)?;
    Ok((b,n,next))
}
/// Main-chain offsets up to `page`, or every distinct one. Like libtiff, a
/// repeated offset ends the count; it hides any later page.
pub fn page_offset(source:&mut Source,header:ct::Header,page:Option<u32>,l:&Limits)->R<Buf<u64>>{
    if page.is_some_and(|p|p as usize>=l.max_directories){return Err((LIMIT,"directory traversal limit"));}
    let mut off=header.first_ifd;let mut seen=Buf::new(l.max_directories,&source.mem)?;
    while off!=0{
        if seen.contains(&off){if page.is_none(){break;}return Err((40,"cyclic directory chain"));}
        if seen.len()==l.max_directories{return Err((LIMIT,"directory traversal limit"));}
        if page==Some(seen.len() as u32){seen.push(off)?;return Ok(seen);}
        // Like libtiff, the count ends before a directory whose link it cannot read.
        let next=match table(source,header,off,l){
            Ok(t)=>t.2,Err(e) if page.is_none()&&e.0==INVALID=>break,Err(e)=>return Err(e),
        };
        seen.push(off)?;off=next;
    }
    if page.is_some(){Err((INVALID,"directory not found"))}else{Ok(seen)}
}
/// Directory bytes, and the bytes in the file the directory occupies: its
/// header, table and out-of-line values, as libtiff counts them when it
/// estimates StripByteCounts. None when an empty record has no type width.
pub fn raw_metadata(source:&mut Source,header:ct::Header,off:u64,l:&Limits,defer:bool)->R<(Buf<u8>,Buf<Tag>,u64,Option<u64>)>{
    let mem=source.mem.clone();let le=header.little_endian;
    let (records,n,next)=table(source,header,off,l)?;
    let ew=if header.big{20}else{12};let cw=if header.big{8}else{2};let slot=if header.big{8}else{4};
    let mut space=(n as u64).checked_mul(ew as u64).and_then(|v|v.checked_add(if header.big{16+8+8}else{8+2+4}));
    let mut entries=Buf::new(n,&mem)?;let mut total=0usize;
    for i in 0..n{
        if records[i*ew..(i+1)*ew].iter().all(|v|*v==0){space=None;continue;}
        let at=add64(off,(cw+i*ew) as u64)?;
        let e=ct::entry(&records[i*ew..i*ew+ew],at,header.big,le,source.len).map_err(err)?;
        if entries.iter().any(|t:&ct::Entry|t.tag==e.tag){return Err((INVALID,"duplicate directory tag"));}
        let len=usize::try_from(e.length).map_err(|_|(LIMIT,"metadata size overflow"))?;
        total=total.checked_add(if defer&&matches!(e.tag,273|279|324|325)&&e.count>1{0}else{len}).and_then(|v|v.checked_add(1)).ok_or((LIMIT,"metadata size overflow"))?;
        if total>l.max_input{return Err((LIMIT,"metadata byte limit"));}
        if e.length>slot{space=space.and_then(|v|v.checked_add(e.length));}
        entries.push(e)?;
    }
    let mut data=Buf::new(total,&mem)?;let mut tags=Buf::new(entries.len(),&mem)?;
    for e in entries.iter(){
        let at=data.len();
        if defer&&matches!(e.tag,273|279|324|325)&&e.count>1{
            tags.push(Tag{key:e.tag,kind:e.kind,count:e.count as usize,at:0,remote:Some(e.offset)})?;continue;
        }
        let raw=source.read(e.offset,e.length as usize)?;
        // Like libtiff, count the NUL added to an ASCII value that lacks one.
        // InkNames keeps its exact payload. The estimate counts the NUL only
        // for an anonymous value or InkNames (tif_dirread.c, TIFFFetchNormalTag).
        let unterminated=e.kind==2&&raw.last().is_some_and(|v|*v!=0);
        if unterminated&&(e.tag==333&&e.count<=65535||field_tags::KNOWN.binary_search(&e.tag).is_err()){
            let out=|n:u64|if n>slot{n}else{0};space=space.and_then(|v|(v-out(e.length)).checked_add(out(e.length+1)));
        }
        let count=e.count as usize+usize::from(unterminated&&e.tag!=333);
        data.extend(&raw)?;data.push(0)?; // ASCII getters are always terminated.
        tags.push(Tag{key:e.tag,kind:e.kind,count,at,remote:None})?;
    }
    Ok((data,tags,next,space))
}
pub fn metadata(source:&mut Source,header:ct::Header,off:u64,l:&Limits,chop:bool,deferred:bool)->R<(Buf<u8>,Meta)>{
    let mem=source.mem.clone();let le=header.little_endian;
    let (data,tags,next,space)=raw_metadata(source,header,off,l,true)?;
    assemble(data,tags,off,next,le,l,chop,&mem,true,Some((source,space,deferred)))
}
/// Image-directory policy: validate the exact InkNames payload, never its
/// extra storage terminator, and publish its actual count as NumberOfInks.
/// Called only on candidate buffers, before a handle's state is replaced.
pub fn normalize_inks(data:&mut Buf<u8>,tags:&mut Buf<Tag>,le:bool,l:&Limits)->R<()>{
    let existing=tags.iter().position(|t|t.key==334);
    let Some(names)=find(tags,333)else{
        if let Some(i)=existing{let t=tags[i];if t.count!=1||value(data,t,0,le)?>65535{return Err((INVALID,"invalid NumberOfInks"));}}
        return Ok(());
    };
    if names.kind!=2||names.remote.is_some(){return Err((INVALID,"InkNames must be ASCII"));}
    let len=checked::payload(data.len(),names.at,names.count,names.kind).map_err(err)?;
    let count=checked::ink_names(&data[names.at..names.at+len]).map_err(err)?;
    if let Some(i)=existing{
        let t=tags[i];
        if t.kind==3&&t.count==1&&value(data,t,0,le)?==count as u64{return Ok(());}
    }
    set_ints(data,tags,334,3,&[count as u64],le,l)
}
/// Replace or insert a SHORT or LONG8 field in candidate storage.
fn set_ints(data:&mut Buf<u8>,tags:&mut Buf<Tag>,key:u16,kind:u16,v:&[u64],le:bool,l:&Limits)->R<()>{
    let existing=tags.iter().position(|t|t.key==key);
    let n=tags.len()+usize::from(existing.is_none());
    if n>l.max_tags{return Err((LIMIT,"directory tag limit"));}
    let w=if kind==3{2}else{8};
    let end=product(v.len(),w)?.checked_add(data.len()).and_then(|e|e.checked_add(1)).ok_or((LIMIT,"metadata size overflow"))?;
    if end>l.max_input{return Err((LIMIT,"metadata byte limit"));}
    tags.reserve(n)?;data.reserve(end)?;
    let at=data.len();
    for x in v{let b=if le{x.to_le_bytes()}else{x.to_be_bytes()};data.extend(if le{&b[..w]}else{&b[8-w..]})?;}
    data.push(0)?;
    let t=Tag{key,kind,count:v.len(),at,remote:None};
    if let Some(i)=existing{tags[i]=t;}else{tags.push(t)?;tags.sort_by_key(|t|t.key);}
    Ok(())
}
/// Like libtiff's directory reader, reject ExtraSamples it cannot set, read
/// Corel's 999 as unassociated alpha, and count samples beyond the
/// photometric's color channels as unspecified extra samples
/// (tif_dir.c setExtraSamples, tif_dirread.c _TIFFGetMaxColorChannels).
/// The stored field keeps its bytes.
fn read_extras(data:&[u8],tags:&[Tag],ch:usize,photo:u64,le:bool,mem:&Mem)->R<Buf<u16>>{
    let t=find(tags,338);let n=t.map_or(0,|t|t.count);
    if n>ch{return Err((INVALID,"ExtraSamples count"));}
    let colors=match photo{0|1|3=>1,2|6|8|9|10|32845=>3,4|5=>4,_=>0};
    let total=if colors>0&&ch-n>colors{ch-colors}else{n};
    let mut v=Buf::new(total,mem)?;
    for i in 0..total{
        let x=if let Some(t)=t.filter(|_|i<n){value(data,t,i,le)?}else{0};
        v.push(match x{0..=2=>x as u16,999=>2,_=>return Err((INVALID,"ExtraSamples value"))})?;
    }
    Ok(v)
}
/// `read` is false for a directory being written. `repair` is the source,
/// directory space and deferred strile loading of a directory being read, as
/// opposed to storage rebuilt after a field change.
pub fn assemble(mut data:Buf<u8>,mut tags:Buf<Tag>,off:u64,next:u64,le:bool,l:&Limits,chop:bool,mem:&Mem,read:bool,repair:Option<(&mut Source,Option<u64>,bool)>)->R<(Buf<u8>,Meta)>{
    normalize_inks(&mut data,&mut tags,le,l)?;
    for key in [256,257,259,262,266,274,277,278,284,317,322,323]{
        if find(&tags,key).is_some_and(|t|t.count!=1){return Err((INVALID,"scalar image field count"));}
    }
    let sc=|key,default|scalar(&data,&tags,key,default,le);
    let width=sc(256,0)? as usize;let height=sc(257,0)? as usize;
    let ch=sc(277,1)? as usize;let bits=sc(258,1)? as usize;let format=u16::try_from(sc(339,1)?).map_err(|_|(INVALID,"SampleFormat overflow"))?;
    let compression=u16::try_from(sc(259,1)?).map_err(|_|(INVALID,"Compression overflow"))?;let predictor=u16::try_from(sc(317,1)?).map_err(|_|(INVALID,"Predictor overflow"))?;
    let extra=if read{read_extras(&data,&tags,ch,sc(262,0)?,le,mem)?}else{small_array(&data,&tags,338,le,ch,mem)?};
    // Without a Photometric tag, libtiff's RGBA reader picks RGB or grayscale
    // from the samples that are not extra samples.
    let colors=ch-extra.len();
    let photo=sc(262,if colors==3{2}else if matches!(compression,2|3|4|32771){0}else{1})?;let planar=sc(284,1)? as usize;
    let orientation=sc(274,1)? as usize;let fill=u16::try_from(sc(266,1)?).map_err(|_|(INVALID,"FillOrder overflow"))?;
    if width==0 || height==0 || width>u32::MAX as usize || height>u32::MAX as usize || ch==0 || ch>64 || bits==0 || bits>128 || (bits>64 && !matches!(format,5|6)) || !(1..=2).contains(&planar) || !(1..=8).contains(&orientation) || !(1..=2).contains(&fill){return Err((INVALID,"invalid image geometry/sample layout"));}
    for key in [258,339]{if let Some(t)=find(&tags,key){if t.count!=1 && t.count!=ch{return Err((INVALID,"per-sample metadata count"));}for i in 1..t.count{if value(&data,t,i,le)?!=value(&data,t,0,le)?{return Err((NOTIMPL,"mixed sample widths/formats"));}}}}
    let tiled=find(&tags,324).is_some();let rows=sc(278,u32::MAX as u64)? as usize;
    let bw=if tiled{sc(322,0)? as usize}else{width};let bh=if tiled{sc(323,0)? as usize}else{rows.min(height)};
    if bw==0 || bh==0 || bw>u32::MAX as usize || bh>u32::MAX as usize{return Err((INVALID,"invalid strip/tile geometry"));}
    let (_,_,_,count)=geometry::grid(width,height,bw,bh,if planar==2{ch}else{1},l.max_segments).map_err(err)?;
    let offsets=Striles::new(&data,&tags,if tiled{324}else{273},le,l.max_segments,mem)?;
    let counts=Striles::new(&data,&tags,if tiled{325}else{279},le,l.max_segments,mem)?;
    // Pixel ranges and streams are deliberately checked only when requested.
    let map=small_array(&data,&tags,320,le,3*65536,&mem)?;
    let subifds=array(&data,&tags,330,le,l.max_directories,&mem)?;
    // Like libtiff, a palette image needs 1 << BitsPerSample ColorMap entries
    // per channel. From 8 bits up it reads as gray or RGB without them.
    let photo=if photo==3&&!(bits<=16&&map.len()==3usize<<bits){
        if bits<8{return Err((INVALID,"palette image without a usable ColorMap"));}
        let fixed=if ch==3{2}else{1};set_ints(&mut data,&mut tags,262,3,&[fixed],le,l)?;fixed
    }else{photo};
    let mut m=Meta{tags,off,next,le,width,height,ch,bits,format,photo,planar,rows,bw,bh,tiled,offsets,counts,map,extra,subifds,compression,predictor,orientation,fill};
    if let Some((source,space,deferred))=repair{
        if let Some(v)=m.estimated_counts(&data,source,space,deferred,count)?{
            let key=if tiled{325}else{279};set_ints(&mut data,&mut m.tags,key,16,&v,le,l)?;
            m.counts=Striles::new(&data,&m.tags,key,le,l.max_segments,mem)?;
        }
    }
    if m.offsets.len()!=count || m.counts.len()!=count{return Err((INVALID,"segment array count mismatch"));}
    if chop && !tiled && planar==1 && compression==1 && count==1{
        // Like libtiff, chop into whole YCbCr sampling rows of about 8 KiB.
        let block=if photo==6{m.subsampling(&data)?.1 as usize}else{1};
        let bytes=m.lib_size(&data,width,Some(block))?;
        let (newrows,stride)=if bytes>8192{(block,bytes)}else{let k=8192/bytes.max(1);(product(k as usize,block)?,k*bytes)};
        if bytes>0&&newrows<rows{
            let n=(height-1)/newrows+1;
            if n>l.max_segments{return Err((LIMIT,"chopped strip count limit"));}
            let pos=m.offsets.values[0];let left=m.counts.values[0];
            m.offsets=Striles::generated(n,pos,stride,left,false,mem)?;
            m.counts=Striles::generated(n,pos,stride,left,true,mem)?;
            m.rows=newrows;m.bh=newrows.min(height);
        }
    }
    Ok((data,m))
}
impl Meta{
    pub fn row_bytes(&self,width:usize)->R<usize>{let ch=if self.planar==2{1}else{self.ch};product(product(width,ch)?,self.bits)?.checked_add(7).map(|n|n/8).ok_or((LIMIT,"row size overflow"))}
    /// Like libtiff's RGBA reader, the kind of alpha: only the first extra
    /// sample can be alpha, and an unspecified one is associated alpha when
    /// there are more than three samples.
    pub fn alpha(&self)->Option<u16>{
        match self.extra.first(){Some(&v@(1|2))=>Some(v),Some(0) if self.ch>3=>Some(1),_=>None}
    }
    pub fn per_plane(&self)->R<usize>{geometry::grid(self.width,self.height,self.bw,self.bh,1,usize::MAX).map(|v|v.2).map_err(err)}
    /// Like libtiff's directory reader, estimate StripByteCounts that are
    /// missing, that look wrong for a single strip, or that differ between the
    /// first two of several uncompressed strips unless strile loading is
    /// deferred (tif_dirread.c, EstimateStripByteCounts). OJPEG keeps its counts.
    fn estimated_counts(&mut self,data:&[u8],source:&mut Source,space:Option<u64>,deferred:bool,count:usize)->R<Option<Buf<u64>>>{
        if self.compression==6||self.offsets.len()!=count{return Ok(None);}
        let file=source.len;let none=self.compression==1;
        if find(&self.tags,if self.tiled{325}else{279}).is_none(){
            if (self.planar==1&&count>1)||(self.planar==2&&count!=self.ch){return Err((INVALID,"missing StripByteCounts"));}
        }else if self.counts.len()!=count{return Ok(None);
        }else if count==1&&!self.tiled{
            let (o,c)=(self.offsets.get(source,0)?,self.counts.get(source,0)?);
            let looks_bad=o!=0&&(c==0||none&&(o<=file&&c>file-o||(self.lib_size(data,self.width,None)?).checked_mul(self.height as u64).is_none_or(|n|c<n)));
            if !looks_bad{return Ok(None);}
        }else{
            if deferred||!(self.planar==1&&count>2&&none){return Ok(None);}
            let (c0,c1)=(self.counts.get(source,0)?,self.counts.get(source,1)?);
            if c0==c1||c0==0||c1==0{return Ok(None);}
        }
        let overflow=(LIMIT,"estimated StripByteCounts overflow");
        let each=if !none{
            let space=space.ok_or((INVALID,"directory record without a type"))?;
            let left=file.saturating_sub(space);
            if self.planar==2{left/self.ch as u64}else{left}
        }else if self.tiled{self.lib_size(data,self.bw,Some(self.bh))?
        }else{
            let per=count/if self.planar==2{self.ch}else{1};
            self.lib_size(data,self.width,None)?.checked_mul((self.height/per.max(1)) as u64).ok_or(overflow)?
        };
        let mut v=Buf::new(count,&source.mem)?;for _ in 0..count{v.push(each)?;}
        if !none{
            // The last strip cannot extend past the end of the file.
            let last=self.offsets.get(source,count-1)?;
            if last.checked_add(each).ok_or(overflow)?>file{v[count-1]=file.saturating_sub(last);}
        }
        Ok(Some(v))
    }
    /// YCbCrSubsampling as libtiff reads it: a pair it cannot read is
    /// ignored in favour of the default.
    pub fn subsampling(&self,data:&[u8])->R<(u64,u64)>{
        let pair=match find(&self.tags,530){Some(t) if t.count==2=>(value(data,t,0,self.le)?,value(data,t,1,self.le)?),_=>(2,2)};
        Ok(if pair.0>0xffff||pair.1>0xffff{(2,2)}else{pair})
    }
    /// libtiff's TIFFScanlineSize64 (`rows` None) or TIFFVTileSize64 for a
    /// segment `w` samples wide, which count YCbCr sampling blocks. Like
    /// libtiff, invalid subsampling or an overflow sizes it as 0.
    pub fn lib_size(&self,data:&[u8],w:usize,rows:Option<usize>)->R<u64>{
        if self.planar==1&&self.photo==6&&self.ch==3{
            let (h,v)=self.subsampling(data)?;
            if !matches!(h,1|2|4)||!matches!(v,1|2|4){return Ok(0);}
            let row=(w as u64).div_ceil(h).checked_mul(h*v+2).and_then(|n|n.checked_mul(self.bits as u64)).map(|n|n.div_ceil(8));
            return Ok(match rows{None=>row.map(|r|r/v),Some(n)=>row.and_then(|r|r.checked_mul((n as u64).div_ceil(v)))}.unwrap_or(0));
        }
        let row=self.row_bytes(w)? as u64;
        Ok(rows.map_or(Some(row),|n|row.checked_mul(n as u64)).unwrap_or(0))
    }
    pub fn segment_rows(&self,k:usize)->R<usize>{if k>=self.offsets.len(){return Err((INVALID,"segment index out of range"));}Ok(if self.tiled{self.bh}else{self.bh.min(self.height-(k%self.per_plane()?)*self.bh)})}
}

/// Offset/count tables stay file-backed until the legacy pointer getter asks
/// for a complete array. A small page cache keeps sequential access efficient.
pub struct Striles{n:usize,remote:Option<(u64,usize,bool)>,generated:Option<(u64,u64,u64,bool)>,pub values:Buf<u64>,page:Option<(usize,Buf<u64>)>}
impl Striles{
    pub fn empty(mem:&Mem)->R<Self>{Ok(Self{n:0,remote:None,generated:None,values:Buf::new(0,mem)?,page:None})}
    pub fn len(&self)->usize{self.n}
    pub fn new(data:&[u8],tags:&[Tag],key:u16,le:bool,cap:usize,mem:&Mem)->R<Self>{
        let mut s=Self::empty(mem)?;let Some(t)=find(tags,key)else{return Ok(s);};
        if t.count>cap{return Err((LIMIT,"segment array limit"));}
        let w=match t.kind{3=>2,4|13=>4,16|18=>8,_=>return Err((INVALID,"segment array type"))};
        s.n=t.count;
        if let Some(off)=t.remote{s.remote=Some((off,w,le));}else{s.values=array(data,tags,key,le,cap,mem)?;}
        Ok(s)
    }
    fn generated(n:usize,off:u64,stride:u64,total:u64,counts:bool,mem:&Mem)->R<Self>{
        let mut s=Self::empty(mem)?;s.n=n;s.generated=Some((off,stride,total,counts));Ok(s)
    }
    pub fn get(&mut self,source:&mut Source,k:usize)->R<u64>{
        if k>=self.n{return Err((INVALID,"segment index out of range"));}
        if self.values.len()==self.n{return Ok(self.values[k]);}
        if let Some((off,stride,total,counts))=self.generated{
            let pos=(k as u64).checked_mul(stride).ok_or((LIMIT,"chopped offset overflow"))?.min(total);
            return if counts{Ok(stride.min(total-pos))}else{add64(off,pos)};
        }
        if let Some((start,page))=&self.page{if k>=*start&&k-*start<page.len(){return Ok(page[k-*start]);}}
        let (off,w,le)=self.remote.ok_or((INVALID,"missing segment array"))?;
        let start=k/256*256;let count=(self.n-start).min(256);
        let raw=source.read(add64(off,product(start,w)? as u64)?,product(count,w)?)?;
        let mut page=Buf::new(count,&source.mem)?;
        for i in 0..count{page.push(read_word(&raw,i*w,w,le).map_err(err)?)?;}
        let v=page[k-start];self.page=Some((start,page));Ok(v)
    }
    pub fn materialize(&mut self,source:&mut Source)->R<()>{
        if self.values.len()==self.n{return Ok(());}
        let mut all=Buf::new(self.n,&source.mem)?;
        for i in 0..self.n{all.push(self.get(source,i)?)?;}
        self.values=all;self.page=None;Ok(())
    }
}
