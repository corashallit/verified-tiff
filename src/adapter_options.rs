//! Checked open options for the C adapter: decimal resource limits from the
//! environment and libtiff's open mode strings. The C adapter reads the
//! environment and passes the caller's strings; it parses neither.
use super::*;
verus! {
/// The value of the first `n` bytes of `s` as decimal digits.
pub open spec fn decimal_value(s:Seq<u8>,n:nat)->int decreases n {
    if n==0 {0} else {decimal_value(s,(n-1) as nat)*10+(s[n-1] as int-48)}
}
pub open spec fn digits(s:Seq<u8>,n:nat)->bool decreases n {
    if n==0 {true} else {digits(s,(n-1) as nat) && 48<=s[n-1]<=57}
}
proof fn decimal_grows(s:Seq<u8>,n:nat)
    requires n<=s.len(),digits(s,n),
    ensures decimal_value(s,n)>=0,
    decreases n,
{
    if n>0 {decimal_grows(s,(n-1) as nat);}
}
/// A limit given in decimal: some digits, and no more than fit a usize.
pub fn decimal(s:&[u8])->(r:Option<usize>)
    ensures r.is_some()==(s.len()>0 && digits(s@,s.len() as nat) && decimal_value(s@,s.len() as nat)<=usize::MAX),
        r.is_some() ==> r.unwrap()==decimal_value(s@,s.len() as nat),
{
    if s.len()==0 {return None;}
    let mut i=0usize;
    let mut n=0usize;
    let mut ok=true;
    // `ok` holds while the digits so far are a value that fits.
    while i<s.len()
        invariant i<=s.len(),
            ok==(digits(s@,i as nat) && decimal_value(s@,i as nat)<=usize::MAX),
            ok ==> n==decimal_value(s@,i as nat),
        decreases s.len()-i,
    {
        let c=s[i];
        proof{
            if digits(s@,i as nat) {decimal_grows(s@,i as nat);}
            assert(digits(s@,(i+1) as nat)==(digits(s@,i as nat) && 48<=c<=57));
            assert(decimal_value(s@,(i+1) as nat)==decimal_value(s@,i as nat)*10+(c as int-48));
        }
        if !ok {
            // A value too large stays too large as digits follow.
            proof{
                if digits(s@,(i+1) as nat) {
                    assert(decimal_value(s@,(i+1) as nat)>=decimal_value(s@,i as nat)) by(nonlinear_arith)
                        requires decimal_value(s@,i as nat)>=0,0<=c as int-48<=9,
                            decimal_value(s@,(i+1) as nat)==decimal_value(s@,i as nat)*10+(c as int-48);
                }
            }
        } else if c<48||c>57 {
            ok=false;
        } else if n>(usize::MAX-(c-48) as usize)/10 {
            proof{
                assert(n as int*10+(c as int-48)>usize::MAX) by(nonlinear_arith)
                    requires n as int>(usize::MAX-(c as int-48))/10,0<=c as int-48<=9;
            }
            ok=false;
        } else {
            proof{
                assert(n as int*10+(c as int-48)<=usize::MAX) by(nonlinear_arith)
                    requires n as int<=(usize::MAX-(c as int-48))/10,0<=c as int-48<=9;
            }
            n=n*10+(c-48) as usize;
        }
        i=i+1;
    }
    if ok {Some(n)} else {None}
}

/// libtiff's mode letters: a read mode is `r` and any of `bcCmMBDOh`; a
/// write mode is `w` and any of `bl8+`.
pub open spec fn read_letter(c:u8)->bool {
    c==98||c==99||c==67||c==109||c==77||c==66||c==68||c==79||c==104
}
pub open spec fn write_letter(c:u8)->bool {c==98||c==108||c==56||c==43}
pub open spec fn letters(s:Seq<u8>,n:nat,write:bool)->bool decreases n {
    if n<=1 {true} else {letters(s,(n-1) as nat,write) && if write {write_letter(s[n-1])} else {read_letter(s[n-1])}}
}
/// Whether a letter after the first `n` bytes' first one is `c`.
pub open spec fn has(s:Seq<u8>,n:nat,c:u8)->bool decreases n {
    if n<=1 {false} else {s[n-1]==c || has(s,(n-1) as nat,c)}
}
/// Whether strips are chopped: by default, and as the last `C` or `c` says.
pub open spec fn chopped(s:Seq<u8>,n:nat)->bool decreases n {
    if n<=1 {true} else if s[n-1]==67 {true} else if s[n-1]==99 {false} else {chopped(s,(n-1) as nat)}
}
pub open spec fn mode_valid(s:Seq<u8>)->bool {
    s.len()>0 && (s[0]==114 && letters(s,s.len(),false) || s[0]==119 && letters(s,s.len(),true))
}
/// The adapter's open flags: chop strips (1), header only (2, `h`), write
/// (4), BigTIFF (8, `8`), big-endian (16, `b`) and deferred strile arrays
/// (32, `D` or `O`).
pub open spec fn mode_value(s:Seq<u8>)->int {
    let n=s.len();
    if s[0]==119 {4+(if has(s,n,56) {8int} else {0})+(if has(s,n,98) {16int} else {0})}
    else {(if chopped(s,n) {1int} else {0})+(if has(s,n,104) {2int} else {0})
        +(if has(s,n,68)||has(s,n,79) {32int} else {0})}
}
pub fn mode_flags(s:&[u8])->(r:Option<u32>)
    ensures r.is_some()==mode_valid(s@),
        r.is_some() ==> r.unwrap()==mode_value(s@),
{
    if s.len()==0||s[0]!=114&&s[0]!=119 {return None;}
    let write=s[0]==119;
    let mut i=1usize;
    let mut ok=true;
    let (mut chop,mut header,mut deferred,mut big,mut be)=(true,false,false,false,false);
    while i<s.len()
        invariant 1<=i<=s.len(),write==(s@[0]==119),
            ok==letters(s@,i as nat,write),chop==chopped(s@,i as nat),
            header==has(s@,i as nat,104),deferred==(has(s@,i as nat,68)||has(s@,i as nat,79)),
            big==has(s@,i as nat,56),be==has(s@,i as nat,98),
        decreases s.len()-i,
    {
        let c=s[i];
        let letter=if write {c==98||c==108||c==56||c==43} else {c==98||c==99||c==67||c==109||c==77||c==66||c==68||c==79||c==104};
        ok=ok&&letter;
        if c==67 {chop=true;} else if c==99 {chop=false;}
        header=header||c==104;
        deferred=deferred||c==68||c==79;
        big=big||c==56;
        be=be||c==98;
        i=i+1;
    }
    if !ok {return None;}
    Some(if write {4+(if big {8} else {0})+(if be {16} else {0})}
        else {(if chop {1} else {0})+(if header {2} else {0})+(if deferred {32} else {0})})
}
}
