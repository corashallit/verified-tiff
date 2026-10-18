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
static int initialise_fields(TIFF *t,unsigned kind){
  t->directory_kind=kind;
  if(kind)return 1;
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
/* One pointer per ColorMap or TransferFunction channel; Rust checks each
   channel's 1 << BitsPerSample entries against the stored length. */
static int channels_getfield(TIFF *t,uint32_t tag,va_list ap,int def){
  uint16_t bits=1,samples=1,extras=0,*extra=NULL;
  TIFFGetFieldDefaulted(t,TIFFTAG_BITSPERSAMPLE,&bits);TIFFGetFieldDefaulted(t,TIFFTAG_SAMPLESPERPIXEL,&samples);TIFFGetFieldDefaulted(t,TIFFTAG_EXTRASAMPLES,&extras,&extra);
  const uint16_t *p[3];size_t n=0;
  int r=vt_channels(t->state,tag,def,bits,samples,extras,p,&n);
  if(r<0){bridge_fail(t,"TIFFGetField");return 0;}
  if(!r)return 0;
  uint16_t **out[3];
  for(size_t i=0;i<n;i++)out[i]=va_arg(ap,uint16_t **);
  for(size_t i=0;i<n;i++)if(!out[i]){fail(t,EINVAL,"TIFFGetField","null output");return 0;}
  for(size_t i=0;i<n;i++)*out[i]=(uint16_t *)p[i];
  return 1;
}
static int generic_getfield(TIFF *t,uint32_t tag,va_list ap,int def){
  const TIFFField *f=find_field(t,tag,TIFF_ANY,1,"TIFFGetField");
  if(!f)return 0; /* Optional absent tags do not raise a decode error. */
  const uint8_t *data=NULL;size_t n=0;
  TIFFDataType kind=f->kind;
  if(!f->size){uint16_t stored=0;vt_field(t->state,tag,&stored,&n);if(!n)return 0;fail(t,ENOSYS,"TIFFGetField","special field convention not implemented");return 0;}
  int r=vt_typed(t->state,tag,(uint16_t)kind,&data,&n);
  if(r<0){bridge_fail(t,"TIFFGetField");return 0;}
  static const double one=1.0;
  static const float white[]={D50_X0/(D50_X0+D50_Y0+D50_Z0),D50_Y0/(D50_X0+D50_Y0+D50_Z0)};
  static const float luma[]={0.299f,0.587f,0.114f};
  if(!r&&def&&tag==TIFFTAG_WHITEPOINT){data=(const uint8_t*)white;n=2;r=1;}
  if(!r&&def&&tag==TIFFTAG_YCBCRCOEFFICIENTS){data=(const uint8_t*)luma;n=3;r=1;}
  if(!r&&def&&tag==TIFFTAG_REFERENCEBLACKWHITE){
    uint16_t bits=1,photo=1;TIFFGetFieldDefaulted(t,TIFFTAG_BITSPERSAMPLE,&bits);TIFFGetFieldDefaulted(t,TIFFTAG_PHOTOMETRIC,&photo);
    float white=(float)(pow(2.,bits)-1.);for(int i=0;i<6;i++)t->default_reference[i]=(i%2)?white:0.;
    if(photo==PHOTOMETRIC_YCBCR){t->default_reference[1]=t->default_reference[3]=t->default_reference[5]=255.;t->default_reference[2]=t->default_reference[4]=128.;}
    data=(const uint8_t*)t->default_reference;n=6;r=1;
  }
  if(!r&&def&&tag==TIFFTAG_STONITS){data=(const uint8_t*)&one;n=1;r=1;}
  if(!r)return 0;
  unsigned plan=vt_field_plan((uint16_t)kind,f->read_count,f->pass_count,(size_t)f->count_size,f->array,(size_t)f->size,n);
  if(!plan){fail(t,EINVAL,"TIFFGetField","field schema/count mismatch");return 0;}
  if(plan==3||plan==4){
    uint32_t *c32=NULL;uint16_t *c16=NULL;
    if(plan==4)c32=va_arg(ap,uint32_t*);else c16=va_arg(ap,uint16_t*);
    void **out=va_arg(ap,void **);if((plan==4?!c32:!c16)||!out)return 0;
    /* Like libtiff, a value with no elements has no data pointer. */
    if(plan==4)*c32=(uint32_t)n;else *c16=(uint16_t)n;*out=n?(void*)data:NULL;
  }else if(plan==2){void **out=va_arg(ap,void **);if(!out)return 0;*out=(void*)data;}
  else{
#define VT_SCALAR(T) do{T *out=va_arg(ap,T*);if(!out)return 0;memcpy(out,data,sizeof(T));}while(0)
    switch(kind){
      case TIFF_BYTE:case TIFF_UNDEFINED:VT_SCALAR(uint8_t);break;
      case TIFF_SBYTE:VT_SCALAR(int8_t);break;
      case TIFF_SHORT:VT_SCALAR(uint16_t);break;
      case TIFF_SSHORT:VT_SCALAR(int16_t);break;
      case TIFF_LONG:case TIFF_IFD:VT_SCALAR(uint32_t);break;
      case TIFF_SLONG:VT_SCALAR(int32_t);break;
      case TIFF_LONG8:case TIFF_IFD8:VT_SCALAR(uint64_t);break;
      case TIFF_SLONG8:VT_SCALAR(int64_t);break;
      case TIFF_FLOAT:VT_SCALAR(float);break;
      case TIFF_DOUBLE:VT_SCALAR(double);break;
      default:return 0;
    }
#undef VT_SCALAR
  }
  return 1;
}
static void unknown_warnings(TIFF *t){
  if(!t->options.warn_unknown)return;
  size_t at=0;uint32_t tag;
  while((tag=vt_unknown_tag(t->state,&at))!=UINT32_MAX)TIFFWarningExtR(t,"TIFFReadDirectory","Unknown field with tag %u",tag);
}
int TIFFReadEXIFDirectory(TIFF *t,uint64_t off){
  if(!valid(t,__func__))return 0;
  if(!vt_custom_dir(t->state,off,1)){bridge_fail(t,__func__);return 0;}return initialise_fields(t,1);
}
int TIFFReadGPSDirectory(TIFF *t,uint64_t off){
  if(!valid(t,__func__))return 0;
  if(!vt_custom_dir(t->state,off,2)){bridge_fail(t,__func__);return 0;}return initialise_fields(t,2);
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
