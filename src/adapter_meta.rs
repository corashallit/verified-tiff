//! Checked metadata transport and schema decisions used by the C adapter.
//! Foreign varargs extraction and pointer validity are separate obligations.
use super::*;
verus! {
/// Count complete NUL-terminated ink names, independently of NumberOfInks.
pub open spec fn ink_zero_count(s:Seq<u8>,n:nat)->nat decreases n {
    if n==0 {0} else {ink_zero_count(s,(n-1) as nat)+if s[n-1]==0 {1nat}else{0nat}}
}
pub open spec fn ink_names_valid(s:Seq<u8>)->bool {
    0<s.len()<=65535 && s[s.len()-1]==0
}
pub fn ink_names(s:&[u8])->(r:Result<u16,Error>)
    ensures r.is_ok()==ink_names_valid(s@),
        r.is_ok() ==> r.unwrap()==ink_zero_count(s@,s.len() as nat) && 1<=r.unwrap()<=65535,
{
    if s.len()==0||s.len()>65535||s[s.len()-1]!=0{return Err(Error::Invalid);}
    let mut i=0usize;let mut count=0u16;
    while i<s.len()
        invariant 0<s.len()<=65535,s@[s.len()-1]==0,i<=s.len(),count<=i,
            count==ink_zero_count(s@,i as nat),
        decreases s.len()-i,
    {
        if s[i]==0{count=count+1;}
        i=i+1;
    }
    assert(ink_zero_count(s@,s.len() as nat)==ink_zero_count(s@,(s.len()-1) as nat)+1);
    Ok(count)
}
pub open spec fn host_kind(k:u16)->u16 {if k==5||k==10 {11} else if k==13 {18} else {k}}
pub fn host_type(k:u16)->(r:u16) ensures r==host_kind(k) {if k==5||k==10 {11} else if k==13 {18} else {k}}
pub open spec fn count_valid(k:u16,fixed:i32,pass:bool,count_width:usize,array:bool,n:usize)->bool {
    container::field_width(k)>0 && k!=5 && k!=10
    && (fixed<=0 || k==2 || n==fixed)
    && (if pass {array && (count_width==2 && n<=65535 || count_width==4 && n<=4294967295)} else {count_width==0 && (array || n==1)})
}
/// The result is a dispatch plan: 0 reject, 1 scalar, 2 pointer, 3 u16 count,
/// 4 u32 count. The file's stored type is deliberately not an input.
pub fn get_plan(k:u16,fixed:i32,pass:bool,count_width:usize,array:bool,n:usize)->(r:u8)
    ensures (r!=0)==count_valid(k,fixed,pass,count_width,array,n),r<=4,
        r==1 ==> !pass && !array && n==1,
        r==2 ==> !pass && array,
        r==3 ==> pass && array && count_width==2 && n<=65535,
        r==4 ==> pass && array && count_width==4 && n<=4294967295,
{
    if container::field_size(k)==0||k==5||k==10||fixed>0&&k!=2&&n!=fixed as usize{return 0;}
    if pass {
        if !array{return 0;}
        if count_width==2&&n<=65535 {3} else if count_width==4&&n<=4294967295 {4} else {0}
    }else{if count_width!=0||!array&&n!=1{0}else if array{2}else{1}}
}
pub open spec fn registration_valid(k:u16,rc:i32,wc:i32,pass:bool)->bool {
    container::field_width(k)>0 && (rc>0||rc== -1||rc== -2||rc== -3)
    && (wc>0||wc== -1||wc== -2||wc== -3)
    && (!pass || (rc== -3)==(wc== -3))
}
pub fn registration(k:u16,rc:i32,wc:i32,pass:bool)->(r:bool)
    ensures r==registration_valid(k,rc,wc,pass),
{container::field_size(k)>0 && (rc>0||rc== -1||rc== -2||rc== -3) && (wc>0||wc== -1||wc== -2||wc== -3) && (!pass || (rc== -3)==(wc== -3))}
/// All integer metadata conversions preserve the mathematical source value.
pub open spec fn signed_kind(k:u16)->bool {k==6||k==8||k==9||k==17}
pub open spec fn integer_kind(k:u16)->bool {k==1||k==2||k==3||k==4||k==6||k==7||k==8||k==9||k==13||k==16||k==17||k==18}
pub open spec fn low(k:u16)->int {if k==6 {-128} else if k==8 {-32768} else if k==9 {-2147483648} else if k==17 {-9223372036854775808} else {0}}
pub open spec fn high(k:u16)->int {if k==6{127}else if k==8{32767}else if k==9{2147483647}else if k==17{9223372036854775807}else if k==1||k==2||k==7{255}else if k==3{65535}else if k==4||k==13{4294967295}else{18446744073709551615}}
pub open spec fn integer_value(v:u64,k:u16)->int {
    if k==6 {(v as i8) as int} else if k==8 {(v as i16) as int} else if k==9 {(v as i32) as int} else if k==17 {(v as i64) as int} else {v as int}
}
pub fn convert_integer(v:u64,source:u16,target:u16)->(r:Result<u64,Error>)
    ensures r.is_ok()==(integer_kind(source)&&integer_kind(target)&&low(target)<=integer_value(v,source)<=high(target)),
        r.is_ok() ==> integer_value(r.unwrap(),target)==integer_value(v,source),
{
    if !(source==1||source==2||source==3||source==4||source==6||source==7||source==8||source==9||source==13||source==16||source==17||source==18)
      || !(target==1||target==2||target==3||target==4||target==6||target==7||target==8||target==9||target==13||target==16||target==17||target==18){return Err(Error::Invalid);}
    let n:i128=if source==6{v as i8 as i128}else if source==8{v as i16 as i128}else if source==9{v as i32 as i128}else if source==17{v as i64 as i128}else{v as i128};
    let lo:i128=if target==6{-128}else if target==8{-32768}else if target==9{-2147483648}else if target==17{-9223372036854775808}else{0};
    let hi:i128=if target==6{127}else if target==8{32767}else if target==9{2147483647}else if target==17{9223372036854775807}else if target==1||target==2||target==7{255}else if target==3{65535}else if target==4||target==13{4294967295}else{18446744073709551615};
    if n<lo||n>hi{return Err(Error::Invalid);}
    proof {
        if target==6 {assert((n as u64) as i8 as i128==n) by(bit_vector) requires -128<=n<=127;}
        else if target==8 {assert((n as u64) as i16 as i128==n) by(bit_vector) requires -32768<=n<=32767;}
        else if target==9 {assert((n as u64) as i32 as i128==n) by(bit_vector) requires -2147483648<=n<=2147483647;}
        else if target==17 {assert((n as u64) as i64 as i128==n) by(bit_vector) requires -9223372036854775808<=n<=9223372036854775807;}
        else {assert(n>=0);}
    }
    Ok(n as u64)
}
/// libtiff implies 2^BitsPerSample entries for each channel of a ColorMap,
/// which stores three, and of a TransferFunction, which stores one curve or
/// three. Callers index each channel by sample value.
pub open spec fn entries(bits:nat)->nat decreases bits {if bits==0 {1} else {2*entries((bits-1) as nat)}}
pub open spec fn channels_valid(colormap:bool,stored:int,bits:nat)->bool {
    bits<=16 && (stored==3*entries(bits) || !colormap && stored==entries(bits))
}
proof fn entries_pow(bits:nat)
    requires bits<=16,
    ensures entries(bits)==crate::deflate_bits::pow(bits as int),
    decreases bits,
{
    crate::deflate_bits::pow_bound(bits as int);
    if bits>0 {entries_pow((bits-1) as nat);}
}
/// A ColorMap has three channel pointers. A TransferFunction has three when
/// there are several color samples, which share a single stored curve.
pub open spec fn channel_parts(colormap:bool,samples:int,extras:int)->int {
    if colormap || samples>extras+1 {3} else {1}
}
/// Channel `i` of `parts` starts at entry `i*stride` and has `each` entries.
pub struct Channels{pub parts:usize,pub each:usize,pub stride:usize}
pub fn channels(colormap:bool,stored:usize,bits:u16,samples:u16,extras:u16)->(r:Result<Channels,Error>)
    ensures r.is_ok()==channels_valid(colormap,stored as int,bits as nat),
        r.is_ok() ==> r.unwrap().each==entries(bits as nat)
            && r.unwrap().parts==channel_parts(colormap,samples as int,extras as int)
            && r.unwrap().stride==(if stored==3*r.unwrap().each {r.unwrap().each} else {0})
            && (r.unwrap().parts-1)*r.unwrap().stride+r.unwrap().each<=stored,
{
    if bits>16{return Err(Error::Invalid);}
    proof{crate::bit_window::powers(bits as u32);crate::deflate_bits::pow_bound(bits as int);entries_pow(bits as nat);}
    let each=(1u32<<bits) as usize;
    let stride=if stored==3*each{each}else if !colormap&&stored==each{0}else{return Err(Error::Invalid);};
    let parts:usize=if colormap||samples as u32>extras as u32+1{3}else{1};
    assert((parts-1)*stride==if parts==3 {2*stride} else {0}) by(nonlinear_arith) requires parts==1||parts==3;
    Ok(Channels{parts,each,stride})
}
/// Validate every source element before the adapter forms a slice. Exact count
/// acceptance excludes both short metadata and spurious always-error code.
pub fn payload(len:usize,at:usize,count:usize,kind:u16)->(r:Result<usize,Error>)
    ensures r.is_ok()==(container::field_width(kind)>0 && at as int+count as int*container::field_width(kind)<=len),
        r.is_ok() ==> r.unwrap()==count as int*container::field_width(kind) && at as int+r.unwrap()<=len,
{
    let w=container::field_size(kind);if w==0{return Err(Error::Invalid);}
    let n=match mul(count,w){Ok(n)=>n,Err(e)=>return Err(e)};
    if !checked_range(len,at,n){return Err(Error::Truncated);}Ok(n)
}
}
