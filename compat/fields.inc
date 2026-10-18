/* Public metadata ABI. The field registry, its schemas and anonymous fields
 * are in compat/src/fields.rs; field values are converted and bounds checked
 * in Rust. C varargs handling remains explicitly unverified. */
static _Atomic(TIFFExtendProc) tag_extender;
TIFFExtendProc TIFFSetTagExtender(TIFFExtendProc p){return atomic_exchange(&tag_extender,p);}
/* The field of a tag, or NULL. `resolve` gives a tag the directory holds an
 * anonymous field, as libtiff's directory reader does. */
static const TIFFField *find_field(TIFF *t,uint32_t tag,TIFFDataType type,int resolve,const char *fn){
  const TIFFField *f=NULL;
  if(vt_find_field(t->state,tag,type,resolve,&f)<0)bridge_fail(t,fn);
  return f;
}
int TIFFMergeFieldInfo(TIFF *t,const TIFFFieldInfo info[],uint32_t n){
  if(!valid(t,__func__))return -1;
  if(!vt_merge_fields(t->state,info,n)){bridge_fail(t,__func__);return -1;}
  return 0; /* libtiff's legacy registration API returns zero on success. */
}
/* A new main directory: the default tag methods, standard fields and the
   application's tag extender. */
static int initialise_fields(TIFF *t){
  t->methods.vgetfield=default_getfield;t->methods.vsetfield=default_setfield;t->methods.printdir=NULL;
  vt_fields_begin(t->state);
  TIFFExtendProc f=atomic_load(&tag_extender);if(f)f(t);
  return !vt_fields_failed(t->state);
}
const TIFFField *TIFFFindField(TIFF *t,uint32_t tag,TIFFDataType type){
  return valid(t,__func__)?find_field(t,tag,type,1,__func__):NULL;
}
const TIFFField *TIFFFieldWithTag(TIFF *t,uint32_t tag){return TIFFFindField(t,tag,TIFF_ANY);}
const TIFFField *TIFFFieldWithName(TIFF *t,const char *name){
  if(!valid(t,__func__)||!name)return NULL;
  const TIFFField *f=NULL;
  if(vt_field_named(t->state,name,&f)<0)bridge_fail(t,__func__);
  return f;
}
uint32_t TIFFFieldTag(const TIFFField *f){return f?f->tag:0;}
const char *TIFFFieldName(const TIFFField *f){return f?f->name:NULL;}
TIFFDataType TIFFFieldDataType(const TIFFField *f){return f?f->type:TIFF_NOTYPE;}
int TIFFFieldPassCount(const TIFFField *f){return f?f->pass_count:0;}
int TIFFFieldReadCount(const TIFFField *f){return f?f->read_count:0;}
int TIFFFieldWriteCount(const TIFFField *f){return f?f->write_count:0;}
int TIFFFieldSetGetSize(const TIFFField *f){return f?f->size:0;}
int TIFFFieldSetGetCountSize(const TIFFField *f){return f?f->count_size:0;}
int TIFFFieldIsAnonymous(const TIFFField *f){return f?f->anonymous:0;}
int TIFFGetTagListCount(TIFF *t){return valid(t,__func__)?(int)vt_custom_tags(t->state):0;}
uint32_t TIFFGetTagListEntry(TIFF *t,int index){
  if(!valid(t,__func__)||index<0)return UINT32_MAX;
  return vt_custom_tag(t->state,(uint32_t)index);
}
static void unknown_warnings(TIFF *t){
  if(!t->options.warn_unknown)return;
  size_t at=0;uint32_t tag;
  while((tag=vt_unknown_tag(t->state,&at))!=UINT32_MAX)TIFFWarningExtR(t,"TIFFReadDirectory","Unknown field with tag %u",tag);
}
int TIFFReadEXIFDirectory(TIFF *t,uint64_t off){
  if(!valid(t,__func__))return 0;
  if(!vt_custom_dir(t->state,off,1)){bridge_fail(t,__func__);return 0;}return 1;
}
int TIFFReadGPSDirectory(TIFF *t,uint64_t off){
  if(!valid(t,__func__))return 0;
  if(!vt_custom_dir(t->state,off,2)){bridge_fail(t,__func__);return 0;}return 1;
}
static int generic_setfield(TIFF *t,uint32_t tag,va_list ap){
  const TIFFField *f=find_field(t,tag,TIFF_ANY,0,"TIFFSetField");
  if(!f){fail(t,ENOSYS,"TIFFSetField","unregistered field");return 0;}
  TIFFDataType kind=f->kind;size_t count=1;const void *input=NULL;
  union {uint64_t u;int64_t i;double d;float f;uint32_t u32;int32_t i32;uint16_t u16;int16_t i16;uint8_t u8;int8_t i8;} scalar;
  uint16_t pair[2];
  if(tag==TIFFTAG_PAGENUMBER||tag==TIFFTAG_YCBCRSUBSAMPLING||tag==TIFFTAG_HALFTONEHINTS||tag==TIFFTAG_DOTRANGE){pair[0]=(uint16_t)va_arg(ap,int);pair[1]=(uint16_t)va_arg(ap,int);count=2;kind=TIFF_SHORT;input=pair;}
  else if(tag==TIFFTAG_COLORMAP){
    uint16_t bits=1;TIFFGetFieldDefaulted(t,TIFFTAG_BITSPERSAMPLE,&bits);if(bits>16){fail(t,EINVAL,"TIFFSetField","palette bit depth");return 0;}
    size_t n=(size_t)1<<bits,bytes=n*6;const uint16_t *a=va_arg(ap,const uint16_t*),*b=va_arg(ap,const uint16_t*),*c=va_arg(ap,const uint16_t*);
    if(!a||!b||!c)return 0;
    if(!vt_mem_claim(t->state,bytes)){bridge_fail(t,"TIFFSetField");return 0;}
    uint16_t *p=malloc(bytes);if(!p){vt_mem_release(t->state,bytes);fail(t,ENOMEM,"TIFFSetField","palette allocation");return 0;}
    memcpy(p,a,n*2);memcpy(p+n,b,n*2);memcpy(p+2*n,c,n*2);int ok=vt_put(t->state,tag,TIFF_SHORT,n*3,(const uint8_t*)p);free(p);vt_mem_release(t->state,bytes);if(!ok)bridge_fail(t,"TIFFSetField");else t->directory_written=0;return ok;
  }
  else if(f->pass_count){count=f->write_count==TIFF_VARIABLE2?va_arg(ap,uint32_t):(uint16_t)va_arg(ap,int);input=va_arg(ap,const void*);}
  else if(kind==TIFF_ASCII){input=va_arg(ap,const char*);if(!input)return 0;count=strnlen(input,t->options.output);if(count==t->options.output){fail(t,EFBIG,"TIFFSetField","string limit");return 0;}count++;}
  else if(f->array||f->write_count>1){if(f->write_count<1){fail(t,ENOSYS,"TIFFSetField","unsized array convention");return 0;}count=(size_t)f->write_count;input=va_arg(ap,const void*);}
  else{
    switch(kind){
    case TIFF_BYTE:case TIFF_UNDEFINED:scalar.u8=(uint8_t)va_arg(ap,int);input=&scalar.u8;break;
    case TIFF_SBYTE:scalar.i8=(int8_t)va_arg(ap,int);input=&scalar.i8;break;
    case TIFF_SHORT:scalar.u16=(uint16_t)va_arg(ap,int);input=&scalar.u16;break;
    case TIFF_SSHORT:scalar.i16=(int16_t)va_arg(ap,int);input=&scalar.i16;break;
    case TIFF_LONG:case TIFF_IFD:scalar.u32=va_arg(ap,uint32_t);input=&scalar.u32;break;
    case TIFF_SLONG:scalar.i32=va_arg(ap,int32_t);input=&scalar.i32;break;
    case TIFF_LONG8:case TIFF_IFD8:scalar.u=va_arg(ap,uint64_t);input=&scalar.u;break;
    case TIFF_SLONG8:scalar.i=va_arg(ap,int64_t);input=&scalar.i;break;
    case TIFF_FLOAT:scalar.f=(float)va_arg(ap,double);input=&scalar.f;break;
    case TIFF_DOUBLE:scalar.d=va_arg(ap,double);input=&scalar.d;break;
    default:fail(t,ENOSYS,"TIFFSetField","field storage convention");return 0;
    }
  }
  if(count&&!input){fail(t,EINVAL,"TIFFSetField","null field input");return 0;}
  int ok=(f->type==TIFF_RATIONAL||f->type==TIFF_SRATIONAL)?vt_put_real(t->state,tag,(uint16_t)f->type,(uint16_t)kind,count,input):vt_put(t->state,tag,(uint16_t)kind,count,input);
  if(!ok)bridge_fail(t,"TIFFSetField");else t->directory_written=0;return ok;
}
