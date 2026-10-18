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
