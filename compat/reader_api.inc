/* Read-side extension hooks. Caller callbacks and foreign pointers are outside
 * the verified core; allocation is charged to the owning TIFF handle. */
struct vt_client_info {struct vt_client_info *next;void *data;size_t charge;char name[];};
static void free_client_info(TIFF *t){
  while(t->client_info){struct vt_client_info *p=t->client_info;t->client_info=p->next;vt_mem_release(t->state,p->charge);free(p);}
}
TIFFTagMethods *TIFFAccessTagMethods(TIFF *t){return valid(t,__func__)?&t->methods:NULL;}
void *TIFFGetClientInfo(TIFF *t,const char *name){
  if(!valid(t,__func__)||!name)return NULL;
  for(struct vt_client_info *p=t->client_info;p;p=p->next)if(!strcmp(p->name,name))return p->data;
  return NULL;
}
void TIFFSetClientInfo(TIFF *t,void *data,const char *name){
  if(!valid(t,__func__)||!name)return;
  for(struct vt_client_info *p=t->client_info;p;p=p->next)if(!strcmp(p->name,name)){p->data=data;return;}
  size_t n=strnlen(name,4096);if(n==4096){fail(t,EINVAL,__func__,"client-info name too long");return;}
  size_t bytes=sizeof(struct vt_client_info)+n+1;
  if(!vt_mem_claim(t->state,bytes)){bridge_fail(t,__func__);return;}
  struct vt_client_info *p=malloc(bytes);
  if(!p){vt_mem_release(t->state,bytes);fail(t,ENOMEM,__func__,"client-info allocation failed");return;}
  p->data=data;p->charge=bytes;memcpy(p->name,name,n+1);p->next=t->client_info;t->client_info=p;
}
int TIFFReadBufferSetup(TIFF *t,void *buffer,tmsize_t size){
  if(!valid(t,__func__))return 0;
  if(size<=0){fail(t,EINVAL,__func__,"invalid read buffer size");return 0;}
  if(!vt_read_buffer(t->state,buffer,(size_t)size)){bridge_fail(t,__func__);return 0;}return 1;
}
int TIFFUnsetField(TIFF *t,uint32_t tag){
  if(!valid(t,__func__)||!TIFFFieldWithTag(t,tag))return 0;
  if(!vt_unset(t->state,tag)){bridge_fail(t,__func__);return 0;}return 1;
}
void TIFFFreeDirectory(TIFF *t){
  if(!valid(t,__func__))return;
  if(!vt_free_directory(t->state)){bridge_fail(t,__func__);return;}
}
static int reader_codec_init(TIFF *t,int scheme){
  uint16_t current=0;
  if(!valid(t,"TIFF codec init")||!TIFFGetFieldDefaulted(t,TIFFTAG_COMPRESSION,&current))return 0;
  if(current!=(uint16_t)scheme){fail(t,EINVAL,"TIFF codec init","codec does not match selected directory");return 0;}
  return TIFFIsCODECConfigured(current);
}
static const TIFFCodec reader_codecs[]={
  {"None",1,reader_codec_init},{"CCITT RLE",2,reader_codec_init},
  {"CCITT Group 3",3,reader_codec_init},{"CCITT Group 4",4,reader_codec_init},
  {"LZW",5,reader_codec_init},{"Old-style JPEG",6,reader_codec_init},
  {"JPEG",7,reader_codec_init},{"AdobeDeflate",8,reader_codec_init},
  {"PackBits",32773,reader_codec_init},{"Deflate",32946,reader_codec_init},
  {"SGILog",34676,reader_codec_init},{"LERC",34887,reader_codec_init},
  {"ZSTD",50000,reader_codec_init},{"WEBP",50001,reader_codec_init},{NULL,0,NULL}
};
const TIFFCodec *TIFFFindCODEC(uint16_t scheme){
  for(size_t i=0;reader_codecs[i].name;i++)if(reader_codecs[i].scheme==scheme)return reader_codecs+i;
  return NULL;
}
TIFFCodec *TIFFGetConfiguredCODECs(void){
  TIFFCodec *p=_TIFFmalloc(sizeof(reader_codecs));if(p)memcpy(p,reader_codecs,sizeof(reader_codecs));return p;
}
void TIFFPrintDirectory(TIFF *t,FILE *file,long flags){
  if(!valid(t,__func__)||!file)return;
  fprintf(file,"TIFF Directory at offset 0x%llx (%llu)\n",(unsigned long long)TIFFCurrentDirOffset(t),(unsigned long long)TIFFCurrentDirOffset(t));
  size_t count=info(t,15);
  for(size_t i=0;i<count;i++){
    uint32_t tag=vt_tag(t->state,i);const TIFFField *field=find_field(t,tag,TIFF_ANY,1,__func__);
    if(!field)continue;
    if(tag==TIFFTAG_STRIPOFFSETS||tag==TIFFTAG_STRIPBYTECOUNTS||tag==TIFFTAG_TILEOFFSETS||tag==TIFFTAG_TILEBYTECOUNTS){
      if(!(flags&TIFFPRINT_STRIPS))continue;
      fprintf(file,"  %s:",field->name);
      for(uint32_t k=0;k<info(t,7);k++)fprintf(file," %llu",(unsigned long long)strile(t,k,tag==TIFFTAG_STRIPBYTECOUNTS||tag==TIFFTAG_TILEBYTECOUNTS,NULL));
      fputc('\n',file);continue;
    }
    const uint8_t *value=NULL;size_t n=0;
    TIFFDataType type=field->kind;
    if(vt_typed(t->state,tag,(uint16_t)type,&value,&n)!=1)continue;
    fprintf(file,"  %s (%u):",field->name,tag);
    size_t max=n<32?n:32;
    if(type==TIFF_ASCII){for(size_t j=0;j<n&&value[j];j++){unsigned c=value[j];if(c>=32&&c<127)fputc(c,file);else fprintf(file,"\\x%02x",c);}}
    else for(size_t j=0;j<max;j++){
      const uint8_t *p=value+j*TIFFDataWidth(type);uint64_t u=0;int64_t v=0;double d=0;float f=0;
      switch(type){
        case TIFF_FLOAT:memcpy(&f,p,4);fprintf(file," %.9g",(double)f);break;
        case TIFF_DOUBLE:memcpy(&d,p,8);fprintf(file," %.17g",d);break;
        case TIFF_SBYTE:v=*(const int8_t*)p;fprintf(file," %lld",(long long)v);break;
        case TIFF_SSHORT:{int16_t x;memcpy(&x,p,2);fprintf(file," %d",x);break;}
        case TIFF_SLONG:{int32_t x;memcpy(&x,p,4);fprintf(file," %d",x);break;}
        case TIFF_SLONG8:memcpy(&v,p,8);fprintf(file," %lld",(long long)v);break;
        default:memcpy(&u,p,(size_t)TIFFDataWidth(type));fprintf(file," %llu",(unsigned long long)u);break;
      }
    }
    if(type!=TIFF_ASCII&&n>max)fprintf(file," ... (%zu values)",n);
    fputc('\n',file);
  }
  if(t->methods.printdir)t->methods.printdir(t,file,flags);
}
/* TIFFFieldArray is opaque in tiffio.h. Programs supplying it use the pinned
 * libtiff internal ABI, which compat/src/fields.rs decodes. */
int TIFFReadCustomDirectory(TIFF *t,toff_t off,const TIFFFieldArray *array){
  if(!valid(t,__func__))return 0;
  if(!vt_read_custom_directory(t->state,off,array)){bridge_fail(t,__func__);return 0;}
  return 1;
}

int TIFFSetFileno(TIFF *t,int fd){if(!valid(t,__func__))return -1;int old=t->fd;t->fd=fd;return old;}
int TIFFSetMode(TIFF *t,int mode){if(!valid(t,__func__))return -1;int old=t->mode;t->mode=mode;return old;}
thandle_t TIFFSetClientdata(TIFF *t,thandle_t client){
  if(!valid(t,__func__))return NULL;
  if(!vt_clientdata(t->state,client)){bridge_fail(t,__func__);return NULL;}
  thandle_t old=t->client;t->client=client;return old;
}
