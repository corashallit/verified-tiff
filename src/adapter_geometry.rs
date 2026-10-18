//! Exact crop placement and strip/tile selection, with no FFI preconditions.
use super::*;
verus! {
pub fn grid(width:usize,height:usize,bw:usize,bh:usize,planes:usize,cap:usize)->(r:Result<(usize,usize,usize,usize),Error>)
    ensures r.is_ok() ==> width>0 && height>0 && bw>0 && bh>0 && planes>0,
        r.is_ok() ==> r.unwrap().0==(width as int-1)/(bw as int)+1 && r.unwrap().1==(height as int-1)/(bh as int)+1,
        r.is_ok() ==> r.unwrap().2==r.unwrap().0 as int*r.unwrap().1 && r.unwrap().3==r.unwrap().2 as int*planes && r.unwrap().3<=cap,
        width>0 && height>0 && bw>0 && bh>0 && planes>0
         && ((width as int-1)/(bw as int)+1)*((height as int-1)/(bh as int)+1)*planes<=cap ==> r.is_ok(),
{
    if width==0||height==0||bw==0||bh==0||planes==0{return Err(Error::Invalid);}
    let across=(width-1)/bw+1;let down=(height-1)/bh+1;
    proof {if across as int*down*planes<=cap {
        assert(across as int*down<=cap) by(nonlinear_arith)
            requires across>0,down>0,planes>0,across as int*down*planes<=cap;
    }}
    let per=match mul(across,down){Ok(v)=>v,Err(e)=>return Err(e)};
    let count=match mul(per,planes){Ok(v)=>v,Err(e)=>return Err(e)};
    if count>cap{return Err(Error::Limit);}Ok((across,down,per,count))
}
pub fn segment(across:usize,down:usize,planes:usize,bx:usize,by:usize,plane:usize)->(r:Result<usize,Error>)
    ensures r.is_ok() ==> bx<across && by<down && plane<planes,
        r.is_ok() ==> r.unwrap()==(plane as int*down+by)*across+bx,
        r.is_ok() ==> r.unwrap()<across as int*down*planes,
        bx<across && by<down && plane<planes && across as int*down*planes<=usize::MAX ==> r.is_ok(),
{
    if bx>=across||by>=down||plane>=planes{return Err(Error::Invalid);}
    proof {
        assert(plane as int*down+by<planes as int*down) by(nonlinear_arith)
            requires plane<planes,by<down,down>0;
        assert((plane as int*down+by)*across+bx<across as int*down*planes) by(nonlinear_arith)
            requires plane as int*down+by<planes as int*down,bx<across,across>0;
        assert(plane as int*down+by<across as int*down*planes) by(nonlinear_arith)
            requires plane as int*down+by<planes as int*down,across>0,down>0,planes>0;
    }
    let p=match mul(plane,down){Ok(v)=>v,Err(e)=>return Err(e)};
    let y=match add(p,by){Ok(v)=>v,Err(e)=>return Err(e)};
    let base=match mul(y,across){Ok(v)=>v,Err(e)=>return Err(e)};
    let k=match add(base,bx){Ok(v)=>v,Err(e)=>return Err(e)};
    proof {assert(k<across as int*down*planes) by(nonlinear_arith)
        requires k==(plane as int*down+by)*across+bx,bx<across,by<down,plane<planes;}
    Ok(k)
}
pub open spec fn span_valid(w:usize,h:usize,vw:usize,vh:usize,x:usize,y:usize,n:usize)->bool {
    0<vw<=w && 0<vh<=h && x as int+n<=vw && y<vh && w as int*h*4<=usize::MAX
}
pub open spec fn span_at(w:usize,h:usize,vw:usize,vh:usize,x:usize,y:usize,n:usize,fx:bool,fy:bool)->int {
    ((h as int-vh+if fy {vh as int-1-y}else{y as int})*w + if fx {vw as int-x-n}else{x as int})*4
}
/// A horizontal span maps to a contiguous destination even when mirrored.
/// The caller reverses its pixels using the already-verified row kernel.
#[inline]
pub fn crop_span(w:usize,h:usize,vw:usize,vh:usize,x:usize,y:usize,n:usize,fx:bool,fy:bool)->(r:Result<(usize,usize),Error>)
    ensures r.is_ok()==span_valid(w,h,vw,vh,x,y,n),
        r.is_ok() ==> r.unwrap().0==span_at(w,h,vw,vh,x,y,n,fx,fy) && r.unwrap().1==n as int*4,
        r.is_ok() ==> r.unwrap().0 as int+r.unwrap().1<=w as int*h*4,
{
    if vw==0||vw>w||vh==0||vh>h||x>vw||n>vw-x||y>=vh{return Err(Error::Invalid);}
    let pixels=match mul(w,h){Ok(v)=>v,Err(e)=>return Err(e)};
    let _bytes=match mul(pixels,4){Ok(v)=>v,Err(e)=>return Err(e)};
    let tx=if fx{vw-x-n}else{x};let ty=h-vh+if fy{vh-1-y}else{y};
    proof {assert((ty as int*w+tx+n)*4<=w as int*h*4) by(nonlinear_arith)
        requires ty<h,tx as int+n<=w,w>0;}
    let at=(ty*w+tx)*4;let count=n*4;Ok((at,count))
}
/// Offset of pixel i inside a span from crop_span: the same destination a
/// one-pixel crop_span at x+i would return, so a span keeps per-pixel placement.
#[inline(always)]
pub fn span_pixel(w:usize,h:usize,vw:usize,vh:usize,x:usize,y:usize,n:usize,fx:bool,fy:bool,i:usize)->(r:usize)
    requires span_valid(w,h,vw,vh,x,y,n),i<n,
    ensures span_at(w,h,vw,vh,x,y,n,fx,fy)+r==span_at(w,h,vw,vh,(x+i) as usize,y,1,fx,fy),r as int+4<=n as int*4,
{
    proof {assert(n as int*4<=w as int*h*4) by(nonlinear_arith) requires n<=w,h>0;}
    if fx {(n-1-i)*4} else {i*4}
}
pub fn orientation(source:usize,target:usize)->(r:Result<(bool,bool),Error>)
    ensures r.is_ok()==(1<=source<=8 && 1<=target<=8),
        r.is_ok() ==> r.unwrap().0==(((source-1)%4==1||(source-1)%4==2)!=((target-1)%4==1||(target-1)%4==2)),
        r.is_ok() ==> r.unwrap().1==(((source-1)%4>=2)!=((target-1)%4>=2)),
{
    if source==0||source>8||target==0||target>8{return Err(Error::Invalid);}
    let s=(source-1)%4;let t=(target-1)%4;
    Ok(((s==1||s==2)!=(t==1||t==2),(s>=2)!=(t>=2)))
}

/// TIFFNumberOfStrips for a tiled image, or a handle with no directory yet:
/// like libtiff, one strip per RowsPerStrip rows of each plane, and a single
/// strip when RowsPerStrip is 0 or covers the image. A count that does not
/// fit 32 bits reads as 0.
pub open spec fn implied_strips(rows: int, height: int, planes: int) -> int {
    (if rows == 0 || rows >= height { 1 } else { (height - 1) / rows + 1 }) * planes
}
/// `planes` is 1 for contiguous samples, and the samples per pixel otherwise.
pub fn strips_from_rows(rows: u64, height: u32, planes: u16) -> (r: u32)
    ensures
        r == (if implied_strips(rows as int, height as int, planes as int) <= u32::MAX {
            implied_strips(rows as int, height as int, planes as int)
        } else {
            0
        }),
{
    let per: u64 = if rows == 0 || rows >= height as u64 {
        1
    } else {
        proof {
            quotient_remainder(height as int - 1, rows as int);
            assert((height as int - 1) / (rows as int) <= height as int - 1) by (nonlinear_arith)
                requires
                    height as int - 1 == rows as int * ((height as int - 1) / (rows as int))
                        + (height as int - 1) % (rows as int),
                    (height as int - 1) % (rows as int) >= 0,
                    (height as int - 1) / (rows as int) >= 0,
                    rows >= 1;
        }
        (height as u64 - 1) / rows + 1
    };
    assert(per as int * planes <= 0x1_0000_0000 * 0xffff) by (nonlinear_arith)
        requires per <= 0x1_0000_0000, planes <= 0xffff;
    let n = per * planes as u64;
    if n > u32::MAX as u64 { 0 } else { n as u32 }
}

/// TIFFCheckTile: like libtiff, a strip image's tiles are its strips, and an
/// image with contiguous samples ignores the sample. `separate` says whether
/// each of the `samples` has its own plane.
pub open spec fn tile_valid(width: u32, height: u32, separate: bool, samples: u16, x: u32, y: u32, z: u32, s: u16) -> bool {
    x < width && y < height && z == 0 && (!separate || s < samples)
}
pub fn check_tile(width: u32, height: u32, separate: bool, samples: u16, x: u32, y: u32, z: u32, s: u16) -> (r: bool)
    ensures r == tile_valid(width, height, separate, samples, x, y, z, s),
{
    x < width && y < height && z == 0 && (!separate || s < samples)
}

/// Tile `x / bw` of row `y / bh` in plane `plane`, as libtiff's
/// TIFFComputeTile numbers it: modulo 2^32, like its 32-bit arithmetic.
pub open spec fn tile_number(width: int, height: int, bw: int, bh: int, plane: int, x: int, y: int) -> int {
    ((plane * ((height - 1) / bh + 1) + y / bh) * ((width - 1) / bw + 1) + x / bw) % 0x1_0000_0000
}
pub fn tile_index(width: u32, height: u32, bw: u32, bh: u32, separate: bool, samples: u16, x: u32, y: u32, z: u32, s: u16)
    -> (r: Result<u32, Error>)
    ensures
        r.is_ok() == (tile_valid(width, height, separate, samples, x, y, z, s) && bw > 0 && bh > 0),
        r.is_ok() ==> r.unwrap() == tile_number(width as int, height as int, bw as int, bh as int,
            if separate { s as int } else { 0 }, x as int, y as int),
{
    if !check_tile(width, height, separate, samples, x, y, z, s) || bw == 0 || bh == 0 {
        return Err(Error::Invalid);
    }
    let across = ((width - 1) / bw) as u128 + 1;
    let down = ((height - 1) / bh) as u128 + 1;
    let plane: u128 = if separate { s as u128 } else { 0 };
    let by = (y / bh) as u128;
    let bx = (x / bw) as u128;
    proof {
        vstd::arithmetic::div_mod::lemma_div_is_ordered_by_denominator(width as int - 1, 1, bw as int);
        vstd::arithmetic::div_mod::lemma_div_is_ordered_by_denominator(height as int - 1, 1, bh as int);
        vstd::arithmetic::div_mod::lemma_div_is_ordered_by_denominator(y as int, 1, bh as int);
        vstd::arithmetic::div_mod::lemma_div_is_ordered_by_denominator(x as int, 1, bw as int);
    }
    assert(plane * down + by <= 0xffff * 0x1_0000_0000 + 0xffff_ffff) by (nonlinear_arith)
        requires plane <= 0xffff, down <= 0x1_0000_0000, by <= 0xffff_ffff;
    assert((plane * down + by) * across + bx <= (0xffff * 0x1_0000_0000 + 0xffff_ffff) * 0x1_0000_0000 + 0xffff_ffff)
        by (nonlinear_arith)
        requires plane * down + by <= 0xffff * 0x1_0000_0000 + 0xffff_ffff, across <= 0x1_0000_0000, bx <= 0xffff_ffff;
    let n = (plane * down + by) * across + bx;
    Ok((n % 0x1_0000_0000) as u32)
}

/// TIFFComputeStrip: strip `row / rows` of plane `plane`, whose strips start
/// `per` apart, modulo 2^32 like libtiff's 32-bit arithmetic.
pub open spec fn strip_number(row: int, rows: int, plane: int, per: int) -> int {
    (row / rows + plane * per) % 0x1_0000_0000
}
/// `rows` is RowsPerStrip, or the strip height of a strip image, and
/// `segments` the strips or tiles of the whole image.
pub fn strip_index(height: u32, rows: u64, segments: u64, separate: bool, samples: u16, row: u32, s: u16)
    -> (r: Result<u32, Error>)
    ensures
        r.is_ok() == (row < height && (!separate || s < samples) && rows > 0),
        r.is_ok() ==> r.unwrap() == strip_number(row as int, rows as int,
            if separate { s as int } else { 0 }, if separate { segments as int / (samples as int) } else { segments as int }),
{
    if row >= height || separate && s >= samples || rows == 0 {
        return Err(Error::Invalid);
    }
    let per = if separate { segments / samples as u64 } else { segments };
    let plane: u128 = if separate { s as u128 } else { 0 };
    proof {
        vstd::arithmetic::div_mod::lemma_div_is_ordered_by_denominator(row as int, 1, rows as int);
    }
    assert(plane * per as u128 <= 0xffff * 0xffff_ffff_ffff_ffff) by (nonlinear_arith)
        requires plane <= 0xffff, per <= 0xffff_ffff_ffff_ffff;
    let n = (row as u64 / rows) as u128 + plane * per as u128;
    Ok((n % 0x1_0000_0000) as u32)
}

/// The first row of a strip, or of a tile in the column of tile rows that
/// `index` counts: like libtiff, rows repeat in each plane.
pub open spec fn first_row(index: int, height: int, bh: int) -> int {
    (index % ((height - 1) / bh + 1)) * bh
}
pub fn segment_row(index: u32, height: u32, bh: u32) -> (r: Result<u32, Error>)
    ensures
        r.is_ok() == (height > 0 && bh > 0),
        r.is_ok() ==> r.unwrap() == first_row(index as int, height as int, bh as int) && r.unwrap() < height,
{
    if height == 0 || bh == 0 {
        return Err(Error::Invalid);
    }
    let per = (height - 1) / bh + 1;
    let k = index % per;
    proof {
        quotient_remainder(height as int - 1, bh as int);
        quotient_remainder(index as int, per as int);
        assert(k as int * bh <= ((height as int - 1) / (bh as int)) * bh) by (nonlinear_arith)
            requires k < per, per == (height as int - 1) / (bh as int) + 1, bh > 0;
    }
    Ok(k * bh)
}
/// The first row of tile `index`, whose tile rows each hold `across` tiles.
pub fn tile_row(index: u32, width: u32, height: u32, bw: u32, bh: u32) -> (r: Result<u32, Error>)
    ensures
        r.is_ok() == (width > 0 && height > 0 && bw > 0 && bh > 0),
        r.is_ok() ==> r.unwrap() == first_row(index as int / ((width as int - 1) / (bw as int) + 1), height as int, bh as int)
            && r.unwrap() < height,
{
    if width == 0 || bw == 0 {
        return Err(Error::Invalid);
    }
    segment_row(index / ((width - 1) / bw + 1), height, bh)
}
}
