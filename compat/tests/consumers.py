"""Consumer API sequences: optional tags, GeoTIFF, EXIF/GPS, temporary writers.
Fixtures are independent Python byte encodings. Pinned libtiff checks serialization.
"""
import itertools, os, pathlib, struct, subprocess, tempfile, unittest
from coverage import OUT, ROOT, scene, assemble, WIDTHS
WIDTHS.update({6:1,8:2,9:4,10:8})

class Consumers(unittest.TestCase):
 def setUp(self):self.tmp=tempfile.TemporaryDirectory();self.path=pathlib.Path(self.tmp.name)
 def tearDown(self):self.tmp.cleanup()
 def call(self,mode,path,*args):
  r=subprocess.run([str(OUT/'consumer-test'),mode,str(path),*map(str,args)],capture_output=True,timeout=30)
  self.assertEqual(r.returncode,0,r.stderr);return r.stdout
 def dump(self,blob,mode='native',oracle=True,expected=None):
  p=self.path/'in.tif';p.write_bytes(blob);a=self.path/'ours';b=self.path/'theirs'
  def run(exe,out):return subprocess.run([str(OUT/exe),str(p),mode,str(out),'0','1'],capture_output=True)
  r=run('dump',a);self.assertEqual(r.returncode,0,r.stderr);data=a.read_bytes()
  if expected is not None:self.assertEqual(data,expected)
  if oracle:
   r=run('dump-libtiff',b);self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(data,b.read_bytes())
  return data
 def test_predictor3(self):
  for bits,ch,le,big,tiled,separate,codec in itertools.product([32,64],[1,3],[False,True],[False,True],[False,True],[False,True],[5,8]):
   blob,native,scan,*_=scene(w=17,h=19,ch=ch,bits=bits,fmt=3,codec=codec,predict=3,le=le,big=big,tiled=tiled,separate=separate)
   with self.subTest(bits=bits,ch=ch,le=le,big=big,tiled=tiled,separate=separate,codec=codec):
    self.dump(blob,expected=native)
    if not tiled:self.dump(blob,'scan',expected=scan)
 def test_cmyk_and_lab(self):
  for photo,ch,bits,le,big,tiled,separate,codec in itertools.product([5,8],[0],[8,16],[False,True],[False,True],[False,True],[False,True],[1,8]):
   ch=4 if photo==5 else 3
   blob,*_=scene(w=17,h=19,photo=photo,ch=ch,bits=bits,le=le,big=big,tiled=tiled,separate=separate,codec=codec)
   with self.subTest(photo=photo,bits=bits,le=le,big=big,tiled=tiled,separate=separate,codec=codec):
    # libtiff's RGBA renderer only implements CMYK8 and contiguous Lab.
    expected=None
    if photo==5:
     # Independent rational CMYK reference, including CMYK16 which the
     # upstream display renderer does not accept.
     expected=bytearray();mask=(1<<bits)-1
     for y in range(19):
      for x in range(17):
       v=[(x*1013+y*3137+c*9109+(mask if (x+y+c)%3==0 else 0))&mask for c in range(4)]
       if bits==16:v=[(n+128)//257 for n in v]
       expected.extend(bytes([(255-v[3])*(255-n)//255 for n in v[:3]]+[255]))
    elif separate:
     # The interleaved source has the exact same logical Lab samples. Use
     # libtiff's supported layout as the independent presentation oracle.
     reference=scene(w=17,h=19,photo=photo,ch=ch,bits=bits,le=le,big=big,tiled=tiled,separate=False,codec=codec)[0]
     rp=self.path/'lab-reference.tif';rp.write_bytes(reference);out=self.path/'lab-reference.rgba'
     r=subprocess.run([str(OUT/'dump-libtiff'),str(rp),'rgba',str(out),'0','1'],capture_output=True)
     self.assertEqual(r.returncode,0,r.stderr);expected=out.read_bytes()
    self.dump(blob,'rgba',oracle=(photo==5 and bits==8) or (photo==8 and not separate),expected=expected)
 def test_ccitt_rle(self):
  from fax_fixtures import one_d,pack
  for le,big,tiled,fill,photo in itertools.product([False,True],[False,True],[False,True],[1,2],[0,1]):
   blob,native,scan,tags,*_=scene(w=17,h=19,ch=1,bits=1,le=le,big=big,tiled=tiled,photo=photo)
   bw=16 if tiled else 17;stride=(bw+7)//8;segs=[];expected=bytearray()
   for key,ty,values in tags:
    if key==(324 if tiled else 273):
     for raw in values[1]:
      rows=[[((raw[y*stride+x//8]>>(7-x%8))&1) for x in range(bw)] for y in range(len(raw)//stride)]
      segs.append(b''.join(pack(one_d(row),fill) for row in rows))
      for row in rows:expected.extend(pack(''.join(map(str,row))))
   tags=[(key,ty,[2] if key==259 else [fill] if key==266 else ('SEG',segs) if key==(324 if tiled else 273) else list(map(len,segs)) if key==(325 if tiled else 279) else values) for key,ty,values in tags]
   with self.subTest(le=le,big=big,tiled=tiled,fill=fill,photo=photo):
    encoded=assemble([tags],big,le)[0];actual=self.dump(encoded,oracle=tiled,expected=bytes(expected))
    if not tiled:
     # libtiff leaves unused low bits as the caller's 0xa5 fill; ours zeroes
     # them. Compare all meaningful native bits and all rendered pixels.
     r=subprocess.run([str(OUT/'dump-libtiff'),str(self.path/'in.tif'),'native',str(self.path/'oracle'),'0','1'],capture_output=True)
     self.assertEqual(r.returncode,0,r.stderr);ref=bytearray((self.path/'oracle').read_bytes())
     for at in range(stride-1,len(ref),stride):ref[at]&=0x80
     self.assertEqual(actual,bytes(ref))
    self.dump(encoded,'rgba')
 def test_ycbcr(self):
  # The independent expected pixels also cover 4x4 with odd edges. Libtiff
  # 4.7.1's 4x4 RGBA edge handling disagrees (its fromskew uses a 4x2 size).
  # Do not reproduce that display bug. The other supported samplings match.
  import zlib
  for hs,vs,tiled,codec,le in itertools.product([1,2,4],[1,2,4],[False,True],[1,8],[False,True]):
   w,h=17,19;bw=16 if tiled else w;bh=16 if tiled else 4;segments=[];rgba=bytearray(w*h*4)
   def rgb(y,cb,cr):
    cb-=128;cr-=128
    return bytes([max(0,min(255,y+((91881*cr+32768)>>16))),max(0,min(255,y+((-22554*cb-46802*cr+32768)>>16))),max(0,min(255,y+((116130*cb+32768)>>16))),255])
   for by in range(0,h,bh):
    for bx in range(0,w,bw):
     rows=bh if tiled else min(bh,h-by);raw=bytearray()
     for y in range(0,rows,vs):
      for x in range(0,bw,hs):
       cb=(x*17+y*3+bx*7+51)%256;cr=(x*3+y*19+by*13+87)%256
       for yy in range(vs):
        for xx in range(hs):
         v=(x+xx+bx+(y+yy+by)*31)%256;raw.append(v)
         if x+xx<bw and bx+x+xx<w and y+yy<rows and by+y+yy<h:
          off=((by+y+yy)*w+bx+x+xx)*4;rgba[off:off+4]=rgb(v,cb,cr)
       raw.extend([cb,cr])
     segments.append(bytes(raw) if codec==1 else zlib.compress(raw))
   tags=[(256,4,[w]),(257,4,[h]),(258,3,[8]*3),(259,3,[codec]),(262,3,[6]),(277,3,[3]),(284,3,[1]),(530,3,[hs,vs]),(324 if tiled else 273,4,('SEG',segments)),(325 if tiled else 279,4,list(map(len,segments)))]
   tags+= [(322,4,[bw]),(323,4,[bh])] if tiled else [(278,4,[bh])]
   with self.subTest(hs=hs,vs=vs,tiled=tiled,codec=codec,le=le):
    self.dump(assemble([tags],le=le)[0],'rgba',oracle=(hs,vs) in [(1,1),(2,1),(2,2),(4,1),(4,2),(1,2)],expected=bytes(rgba))
 def test_temporary_decoder_context(self):
  prefix=self.path/'temporary';self.call('writer','unused',prefix)
  # libtiff can reopen the metadata-only images and returns their dimensions.
  for p in self.path.glob('temporary-*.tif'):
   r=subprocess.run([str(ROOT/'.tools/tiff-4.7.1/tools/tiffinfo'),str(p)],capture_output=True)
   self.assertEqual(r.returncode,0,r.stderr);self.assertIn(b'Image Width: 16 Image Length: 16',r.stdout)
 def test_environment_limits(self):
  p=self.path/'limited.tif';p.write_bytes(scene()[0])
  for key,value,expected in [('VTIFF_MAX_FILE_BYTES','1',0),('VTIFF_MAX_FILE_BYTES','garbage',0),('VTIFF_MAX_BUFFER_BYTES','0',1),('VTIFF_MAX_WORK','0',1),('VTIFF_MAX_SINGLE_ALLOC','1',0),('VTIFF_MAX_TOTAL_ALLOC','1',0),('VTIFF_MAX_SINGLE_ALLOC','0',2)]:
   r=subprocess.run([str(OUT/'consumer-test'),'environment',str(p),str(expected)],env={**os.environ,key:value},capture_output=True)
   self.assertEqual(r.returncode,0,(key,r.stderr))
  for override in [0,1]:
   # Explicit settings repair only the environment values they replace.
   env={**os.environ,**{k:'invalid' for k in ['VTIFF_MAX_FILE_BYTES','VTIFF_MAX_BUFFER_BYTES','VTIFF_MAX_WORK','VTIFF_MAX_SINGLE_ALLOC','VTIFF_MAX_TOTAL_ALLOC']}}
   r=subprocess.run([str(OUT/'consumer-test'),'environment-override',str(p),str(override)],env=env,capture_output=True)
   self.assertEqual(r.returncode,0,r.stderr)
 def test_transfer_function(self):
  for le,big in itertools.product([False,True],repeat=2):
   values=list(range(0,65536,257))+list(range(65535,-1,-257))+[17]*256
   blob=scene(le=le,big=big,extra=[(301,3,values)])[0];p=self.path/'transfer.tif';p.write_bytes(blob);self.call('transfer',p)
 def test_registered_anonymous_and_nonimage_directories(self):
  for big,le in itertools.product([False,True],repeat=2):
   e='<' if le else '>'
   tags=scene()[3]
   tags+= [(33550,12,struct.pack(e+'3d',2.5,3.5,0)),(34735,3,[1,1,0,1,1024,0,1,1]),(34737,2,b'WGS 84|\0'),(65001,9,struct.pack(e+'3i',-123,0,2147483647)),(65002,11,struct.pack(e+'2f',1.25,-3.5)),(34665,18 if big else 13,('SUB',[1])),(34853,18 if big else 13,('SUB',[2]))]
   exif=[(33434,5,struct.pack(e+'II',1,8)),(36867,2,b'2026:10:15 12:34:56\0')]
   gps=[(0,1,bytes([2,3,0,0])),(1,2,b'N\0'),(2,5,struct.pack(e+'6I',51,1,30,1,0,1))]
   p=self.path/'metadata.tif';p.write_bytes(assemble([tags,exif,gps],big,le)[0]);self.call('metadata',p)
 def test_temporary_compressed_contexts(self):
  for name in ['gray-strip.tif','rgb-be-tile.tif','lzw-rgb-tile.tif','alpha-4-1-none.tif','fax-g3.tif','fax-g4-tile.tif','webp-tile.tif','logluv-strip.tif']:
   self.call('clone',ROOT/'tests/fixtures'/name)
  for bits,predict in [(8,2),(32,3),(64,3)]:
   for le,tiled in itertools.product([False,True],repeat=2):
    p=self.path/'codec.tif';p.write_bytes(scene(w=17,h=19,ch=3,bits=bits,fmt=3 if predict==3 else 1,codec=8,predict=predict,le=le,tiled=tiled)[0]);self.call('clone',p)
 def test_rgba_caller_source_orientation(self):
  # A caller-set TIFFRGBAImage.orientation replaces the stored Orientation tag.
  for ch,photo in [(1,None),(3,None),(4,5)]:
   out=[]
   for orientation in [1,4,6]:
    p=self.path/'oriented.tif';p.write_bytes(scene(w=17,h=19,ch=ch,photo=photo,orientation=orientation)[0]);out.append(self.call('rgba-as-stored',p))
   with self.subTest(ch=ch):self.assertEqual(out[0],out[1]);self.assertEqual(out[0],out[2])
 def test_eight_bit_colormaps(self):
  # libtiff treats a map with no entry above 255 as 8-bit and others as 16-bit.
  for bits,scale in itertools.product([2,8],[1,257]):
   n=1<<bits;cmap=[(i*37+c*101)%256*scale for c in range(3) for i in range(n)]
   blob=scene(w=17,h=19,ch=1,bits=bits,photo=3,extra=[(320,3,cmap)])[0]
   with self.subTest(bits=bits,scale=scale):self.dump(blob,'rgba')
 def test_palette_without_usable_colormap(self):
  # libtiff ignores a ColorMap without 1 << BitsPerSample entries per channel.
  # An 8-bit palette image then reads as grayscale; a 4-bit one fails to open.
  for cmap in [None,[1,2,3]]:
   extra=[] if cmap is None else [(320,3,cmap)]
   with self.subTest(cmap=cmap):
    self.dump(scene(w=17,h=19,ch=1,bits=8,photo=3,extra=extra)[0],'rgba')
    p=self.path/'in.tif';p.write_bytes(scene(w=17,h=19,ch=1,bits=4,photo=3,extra=extra)[0])
    for exe in ['dump','dump-libtiff']:
     self.assertEqual(subprocess.run([str(OUT/exe),str(p),'rgba',str(self.path/'out')],capture_output=True).returncode,3)
 def test_directory_reader_implied_fields(self):
  # Like libtiff, Compression and PlanarConfig read back even when absent.
  self.call('implied',ROOT/'tests/fixtures/profile-witness.tif')
 def test_directory_loops_end_iteration(self):
  # libtiff counts each distinct IFD once and stops a ReadDirectory loop there.
  for name,pages in [('test_ifd_loop_to_self.tif',1),('test_ifd_loop_to_first.tif',2),('test_ifd_loop_subifd.tif',5)]:
   self.call('pages',ROOT/'tests/upstream'/name,pages)
 def test_unreadable_directory_ends_count(self):
  # libtiff counts the directories before one whose link it cannot read.
  for big in [False,True]:
   tags=scene(big=big)[3];blob=bytearray(assemble([tags,tags],big,main_next=[1,None])[0])
   cw,ew,slot=(8,20,8) if big else (2,12,4);link=(16 if big else 8)+2*(cw+ew*len(tags))+slot
   struct.pack_into('<Q' if big else '<I',blob,link,len(blob)+64)
   p=self.path/'tail.tif';p.write_bytes(blob)
   with self.subTest(big=big):self.call('pages',p,2)
 def test_file_sized_input_limit(self):
  # Single-segment codecs decode a derived view that is larger than a tiny file.
  for name in ['fax-g3.tif','fax-g4-tile.tif','jpeg-gray.tif','jpeg-tile.tif','ojpeg-tables.tif','webp-tile.tif','logluv-strip.tif']:
   self.call('exact-limit',ROOT/'tests/fixtures'/name)
 def test_strip_queries_on_tiled_images(self):
  # libtiff answers strip counts and sizes from RowsPerStrip on tiled images too.
  for name in ['acceptance-gray16.tif','deflate-planar16.tif','lzw-rgb-tile.tif','rgb16.tif','planar-alpha.tif','float64-planar.tif','palette4.tif','fax-g4-tile.tif','jpeg-tile.tif','jpeg-ycbcr.tif']:
   with self.subTest(name=name):self.dump((ROOT/'tests/fixtures'/name).read_bytes(),'sizes')
  # One-row strips of a tall two-plane image overflow libtiff's 32-bit count.
  tags=[(256,4,[16]),(257,4,[0x80000008]),(258,3,[8,8]),(259,3,[1]),(262,3,[1]),(277,3,[2]),(278,4,[1]),(284,3,[2]),
   (322,4,[16]),(323,4,[0x7ffffff0]),(324,4,('SEG',[b'\0']*4)),(325,4,[1]*4)]
  self.assertEqual(self.dump(assemble([tags])[0],'sizes').split()[0],b'0')
 def test_unknown_tag_counts(self):
  # libtiff counts the NUL it adds to an unterminated ASCII value, and an
  # empty value has no data pointer.
  for kind,value in [(2,b'unterminated'),(2,b'a\0b'),(2,b'done\0'),(2,b''),(1,b''),(1,b'\1\2')]:
   with self.subTest(kind=kind,value=value):self.dump(scene(extra=[(65000,kind,value)])[0],'tag65000')
 def test_read_side_sample_format(self):
  for bits,le,big in itertools.product([32,64],[False,True],[False,True]):
   p=self.path/'float.tif';p.write_bytes(scene(bits=bits,fmt=3,le=le,big=big,codec=8)[0]);self.call('readset',p)
 def test_logluv_float(self):
  import math
  for name in ['logluv-strip.tif','logluv-pages.tif']:
   p=ROOT/'tests/fixtures'/name;ours=self.call('logluv',p)
   r=subprocess.run([str(OUT/'consumer-libtiff'),'logluv',str(p)],capture_output=True)
   self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(len(ours),len(r.stdout))
   for (a,),(b,) in zip(struct.iter_unpack('=f',ours),struct.iter_unpack('=f',r.stdout)):
    self.assertTrue(math.isclose(a,b,rel_tol=2e-7,abs_tol=1e-30),(a,b))
if __name__=='__main__':unittest.main(verbosity=2)
