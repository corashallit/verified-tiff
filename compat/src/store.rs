//! Field value storage with libtiff's pointer lifetimes: a value a getter
//! hands out stays where it is until that field changes or another directory
//! is selected.
use super::*;
use verified_tiff::adapter_meta as checked;

/// Value bytes by tag, from an assembly arena or the resident store.
pub trait Values{fn bytes(&self,t:&Tag)->&[u8];}
impl Values for [u8]{fn bytes(&self,_:&Tag)->&[u8]{self}}
impl Values for Buf<u8>{fn bytes(&self,_:&Tag)->&[u8]{self}}
impl Values for Store{fn bytes(&self,t:&Tag)->&[u8]{self.slots.get(t.slot).and_then(|s|s.as_deref()).unwrap_or(&[])}}

/// The selected directory's values. A tag's `slot` names the allocation that
/// holds its bytes: slot 0 is the directory as read, and each value changed
/// since then has a slot of its own. Values are followed by a NUL, so ASCII
/// getters are always terminated.
pub struct Store{slots:Buf<Option<Buf<u8>>>}
enum Place{Kept(usize,usize),Fresh(usize),Remote}
impl Store{
    pub fn new(arena:Buf<u8>,mem:&Mem)->R<Self>{let mut slots=Buf::new(1,mem)?;slots.push(Some(arena))?;Ok(Self{slots})}
    pub fn len(&self)->usize{self.slots.iter().flatten().map(|s|s.len()).sum()}
    fn payload<'a>(&'a self,t:&Tag)->R<&'a [u8]>{
        let s=self.bytes(t);let n=checked::payload(s.len(),t.at,t.count,t.kind).map_err(err)?;Ok(&s[t.at..t.at+n])
    }
    /// The values of `tags`, less `skip`, copied into one arena to rebuild the
    /// directory from, with room for `extra` more bytes and one more tag.
    pub fn arena(&self,tags:&[Tag],skip:Option<u16>,extra:usize,mem:&Mem)->R<(Buf<u8>,Buf<Tag>)>{
        let mut total=extra;
        for t in tags.iter().filter(|t|t.remote.is_none()&&Some(t.key)!=skip){
            total=total.checked_add(self.payload(t)?.len()).and_then(|v|v.checked_add(1)).ok_or((LIMIT,"metadata size overflow"))?;
        }
        let mut data=Buf::new(total,mem)?;let mut out=Buf::new(tags.len()+1,mem)?;
        for t in tags.iter().filter(|t|Some(t.key)!=skip){
            if t.remote.is_some(){out.push(*t)?;continue;}
            let at=data.len();data.extend(self.payload(t)?)?;data.push(0)?;out.push(Tag{at,slot:0,..*t})?;
        }
        Ok((data,out))
    }
    /// Take the values of a directory rebuilt in `arena`. A value equal to the
    /// one `old` had keeps its storage, so pointers to it stay valid. Any other
    /// value gets a slot of its own, and a slot no tag uses any more is freed.
    /// Nothing changes on failure.
    pub fn rebase(&mut self,arena:&[u8],tags:&mut [Tag],old:&[Tag],mem:&Mem)->R<()>{
        let mut places=Buf::new(tags.len(),mem)?;let mut fresh=Buf::new(tags.len(),mem)?;
        for t in tags.iter(){
            if t.remote.is_some(){places.push(Place::Remote)?;continue;}
            let n=checked::payload(arena.len(),t.at,t.count,t.kind).map_err(err)?;let bytes=&arena[t.at..t.at+n];
            let same=|o:&&Tag|o.key==t.key&&o.kind==t.kind&&o.count==t.count&&o.remote.is_none()&&self.payload(o).is_ok_and(|v|v==bytes);
            if let Some(o)=old.iter().find(same){places.push(Place::Kept(o.slot,o.at))?;continue;}
            let mut b=Buf::new(n+1,mem)?;b.extend(bytes)?;b.push(0)?;places.push(Place::Fresh(fresh.len()))?;fresh.push(Some(b))?;
        }
        let live=|s:usize|places.iter().any(|p|matches!(p,Place::Kept(x,_) if *x==s));
        let free=(0..self.slots.len()).filter(|s|!live(*s)).count();
        self.slots.reserve(self.slots.len()+fresh.len().saturating_sub(free))?;
        for s in 0..self.slots.len(){if !live(s){self.slots[s]=None;}}
        for (t,p) in tags.iter_mut().zip(places.iter()){
            match *p{
                Place::Kept(slot,at)=>{t.slot=slot;t.at=at;}
                Place::Fresh(i)=>{
                    let value=fresh[i].take();
                    t.slot=match self.slots.iter().position(|s|s.is_none()){Some(s)=>{self.slots[s]=value;s},None=>{self.slots.push(value)?;self.slots.len()-1}};t.at=0;
                }
                Place::Remote=>{}
            }
        }
        Ok(())
    }
    /// Free the slots `tags` no longer use, as after a field is unset.
    pub fn release(&mut self,tags:&[Tag]){
        for s in 0..self.slots.len(){if !tags.iter().any(|t|t.remote.is_none()&&t.slot==s){self.slots[s]=None;}}
    }
}
