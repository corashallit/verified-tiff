/* Boundary unit tests compile the unchanged production C source. Allocation
 * failures affect C allocations only. Linker wrappers inject documented bridge
 * failures and defensive boundary values; ordinary calls use the real Rust
 * implementation. These unit results are reported separately from integration. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static long alloc_fail=-1,alloc_calls;
static size_t alloc_bytes;
static int allocation_fails(void){return alloc_fail>=0 && alloc_calls++==alloc_fail;}
static void *owned[16384];static size_t owned_count;
static void track(void*p){if(p){assert(owned_count<16384);owned[owned_count++]=p;}}
static void untrack(void*p){for(size_t i=0;i<owned_count;i++)if(owned[i]==p){owned[i]=owned[--owned_count];return;}}
static void *shim_malloc(size_t n){alloc_bytes=n;void*p=allocation_fails()?NULL:malloc(n);track(p);return p;}
static void *shim_calloc(size_t n,size_t s){alloc_bytes=n*s;void*p=allocation_fails()?NULL:calloc(n,s);track(p);return p;}
static void *shim_realloc(void *p,size_t n){alloc_bytes=n;if(allocation_fails())return NULL;untrack(p);void*q=realloc(p,n);if(q)track(q);else track(p);return q;}
static void shim_free(void*p){untrack(p);free(p);}
#define malloc shim_malloc
#define calloc shim_calloc
#define realloc shim_realloc
#define free shim_free
#include "../tiff_compat.c"
#undef malloc
#undef calloc
#undef realloc
#undef free
static const char *fault;
static uint32_t fault_tag;
static int fault_rc;
static uint64_t fault_value;
static int injecting(const char *name,uint32_t tag){return fault && !strcmp(fault,name) && (!fault_tag||fault_tag==tag);}
static void inject(const char *name,int rc){fault=name;fault_tag=0;fault_rc=rc;fault_value=0;}
static void clear_fault(void){fault=NULL;alloc_fail=-1;alloc_calls=0;}
int __real_vt_error_code(void);
int __wrap_vt_error_code(void){return fault?EIO:__real_vt_error_code();}
const char *__real_vt_error_message(void);
const char *__wrap_vt_error_message(void){return fault?"injected bridge failure":__real_vt_error_message();}
#define WRAP_INT1(n) int __real_##n(void*); int __wrap_##n(void *p){return injecting(#n,0)?fault_rc:__real_##n(p);}
#define WRAP_INT2(n,T) int __real_##n(void*,T); int __wrap_##n(void *p,T v){return injecting(#n,0)?fault_rc:__real_##n(p,v);}
WRAP_INT1(vt_next) WRAP_INT1(vt_free_directory) WRAP_INT1(vt_rgba_ok)
WRAP_INT2(vt_select,uint32_t) WRAP_INT2(vt_set_offset,uint64_t) WRAP_INT2(vt_unset,uint32_t) WRAP_INT2(vt_clientdata,thandle_t) WRAP_INT2(vt_rgba_begin,struct vt_rgba*)
uint64_t __real_vt_info(void*,uint32_t);
uint64_t __wrap_vt_info(void*p,uint32_t k){return injecting("vt_info",k)?fault_value:__real_vt_info(p,k);}
int64_t __real_vt_strips(void*);
int64_t __wrap_vt_strips(void*p){return injecting("vt_strips",0)?(int64_t)fault_value:__real_vt_strips(p);}
uint32_t __real_vt_count_dirs(void*);
uint32_t __wrap_vt_count_dirs(void*p){return injecting("vt_count_dirs",0)?(uint32_t)fault_value:__real_vt_count_dirs(p);}
int64_t __real_vt_size(void*,uint32_t,uint32_t);
int64_t __wrap_vt_size(void*p,uint32_t k,uint32_t n){return injecting("vt_size",k)?(int64_t)fault_value:__real_vt_size(p,k,n);}
int __real_vt_get(void*,uint32_t,int,struct vt_plan*);
int __wrap_vt_get(void*p,uint32_t k,int d,struct vt_plan*o){return injecting("vt_get",k)?fault_rc:__real_vt_get(p,k,d,o);}
int __real_vt_strile(void*,uint32_t,int,uint64_t*);
int __wrap_vt_strile(void*p,uint32_t k,int c,uint64_t*v){if(injecting("vt_strile",k)){*v=fault_value;return fault_rc;}return __real_vt_strile(p,k,c,v);}
int __real_vt_custom_dir(void*,uint64_t,uint32_t);
int __wrap_vt_custom_dir(void*p,uint64_t o,uint32_t k){return injecting("vt_custom_dir",0)?fault_rc:__real_vt_custom_dir(p,o,k);}
int __real_vt_find_field(void*,uint32_t,TIFFDataType,int,const TIFFField**);
int __wrap_vt_find_field(void*p,uint32_t k,TIFFDataType ty,int r,const TIFFField**f){if(injecting("vt_find_field",k)){*f=NULL;return fault_rc;}return __real_vt_find_field(p,k,ty,r,f);}
int __real_vt_field_named(void*,const char*,const TIFFField**);
int __wrap_vt_field_named(void*p,const char*n,const TIFFField**f){if(injecting("vt_field_named",0)){*f=NULL;return fault_rc;}return __real_vt_field_named(p,n,f);}
static const char *gray_path,*tile_path;
static TIFF *gray(void){TIFF*t=TIFFOpen(gray_path,"rc");assert(t);return t;}
static unsigned diagnostics,extended,local,recurse;
static void diagnostic(const char*m,const char*f,va_list a){(void)m;char b[128];vsnprintf(b,sizeof(b),f,a);assert(b[0]);diagnostics++;if(recurse){recurse=0;TIFFWarning(NULL,"nested");}}
static void diagnostic_ext(thandle_t c,const char*m,const char*f,va_list a){(void)c;diagnostic(m,f,a);extended++;}
static int diagnostic_local(TIFF*t,void*u,const char*m,const char*f,va_list a){(void)t;(void)m;(void)f;(void)a;local++;return *(int*)u;}
static void diagnostics_test(void){
 TIFFError(NULL,"default %d",1);TIFFWarning("module","default %d",2);
 TIFFErrorHandler old=TIFFSetErrorHandler(diagnostic),ow=TIFFSetWarningHandler(diagnostic);
 assert(TIFFSetErrorHandlerExt(diagnostic_ext)==NULL);assert(TIFFSetWarningHandlerExt(diagnostic_ext)==NULL);
 TIFFError("test","%d",1);TIFFWarning("test","%s","warning");TIFFErrorExt((void*)1,NULL,"extended");TIFFWarningExt(NULL,NULL,"extended");
 TIFFErrorExtR(NULL,NULL,"null handle");TIFFWarningExtR(NULL,NULL,"null handle");assert(extended==6);
 recurse=1;TIFFError(NULL,"outer");assert(!recurse);
 TIFF*t=gray();int handled=0;t->options.error=diagnostic_local;t->options.warning=diagnostic_local;t->options.error_data=&handled;t->options.warning_data=&handled;
 TIFFErrorExtR(t,NULL,"local");TIFFWarningExtR(t,NULL,"local");handled=1;unsigned before=diagnostics;TIFFErrorExtR(t,NULL,"handled");TIFFWarningExtR(t,NULL,"handled");assert(diagnostics==before&&local==4);TIFFClose(t);
 TIFFSetErrorHandler(NULL);TIFFSetWarningHandler(NULL);TIFFSetErrorHandlerExt(NULL);TIFFSetWarningHandlerExt(NULL);TIFFError(NULL,"silent");TIFFWarning(NULL,"silent");
 assert(strstr(TIFFGetVersion(),"verified-tiff"));VTIFFClearError();assert(!*VTIFFLastError()&&!VTIFFLastErrorCode());(void)old;(void)ow;
}
static void utilities_test(void){
 assert(!_TIFFmalloc(-1));alloc_fail=0;alloc_calls=0;assert(!_TIFFmalloc(1)&&errno==ENOMEM);clear_fault();
 void*p=_TIFFmalloc(0);assert(p);memset(p,42,1);assert(!_TIFFrealloc(p,-1));alloc_fail=0;alloc_calls=0;assert(!_TIFFrealloc(p,2));assert(*(char*)p==42);clear_fault();p=_TIFFrealloc(p,0);assert(p);_TIFFfree(p);
 assert(!_TIFFcalloc(-1,1));assert(!_TIFFcalloc(1,-1));assert(!_TIFFcalloc(PTRDIFF_MAX,2));p=_TIFFcalloc(0,PTRDIFF_MAX);assert(p&&alloc_bytes==1);_TIFFfree(p);p=_TIFFcalloc(2,0);assert(p&&alloc_bytes==1);_TIFFfree(p);p=_TIFFcalloc(2,3);assert(p&&!memcmp(p,"\0\0\0\0\0",6));_TIFFfree(p);
 alloc_fail=0;alloc_calls=0;assert(!_TIFFcalloc(1,1));clear_fault();
 unsigned char a[32],b[32];memset(a,0xa5,sizeof(a));memset(b,0xa5,sizeof(b));
 _TIFFmemset(a,0,-1);assert(a[0]==0xa5);_TIFFmemset(NULL,0,1);_TIFFmemset(NULL,0,0);_TIFFmemset(a,3,4);assert(a[3]==3&&a[4]==0xa5);
 _TIFFmemcpy(a,b,-1);_TIFFmemcpy(NULL,b,1);_TIFFmemcpy(a,NULL,1);_TIFFmemcpy(NULL,NULL,0);_TIFFmemcpy(a,b,4);assert(!memcmp(a,b,32));
 assert(!_TIFFmemcmp(a,b,-1));assert(!_TIFFmemcmp(NULL,b,1));assert(!_TIFFmemcmp(a,NULL,1));assert(!_TIFFmemcmp(NULL,NULL,0));assert(!_TIFFmemcmp(a,b,32));a[0]=1;assert(_TIFFmemcmp(a,b,1)<0);
 #define CHECK_SWAP(Name,Type,W) do{Type v[2];unsigned char orig[sizeof(v)];for(size_t j=0;j<sizeof(v);j++)orig[j]=(unsigned char)(j+1);memcpy(v,orig,sizeof(v));TIFFSwab##Name(v);for(size_t j=0;j<W;j++)assert(((unsigned char*)v)[j]==orig[W-1-j]);TIFFSwab##Name(v);assert(!memcmp(v,orig,sizeof(v)));TIFFSwabArrayOf##Name(v,2);for(size_t k=0;k<2;k++)for(size_t j=0;j<W;j++)assert(((unsigned char*)v)[k*W+j]==orig[k*W+W-1-j]);TIFFSwabArrayOf##Name(NULL,0);TIFFSwabArrayOf##Name(NULL,1);TIFFSwabArrayOf##Name(v,-1);TIFFSwabArrayOf##Name(v,PTRDIFF_MAX);}while(0)
 CHECK_SWAP(Short,uint16_t,2);CHECK_SWAP(Long,uint32_t,4);CHECK_SWAP(Long8,uint64_t,8);CHECK_SWAP(Float,float,4);CHECK_SWAP(Double,double,8);
 uint8_t triples[]={1,2,3,4,5,6};TIFFSwabArrayOfTriples(triples,2);assert(!memcmp(triples,"\3\2\1\6\5\4",6));
 TIFFReverseBits(NULL,0);TIFFReverseBits(NULL,1);TIFFReverseBits(a,-1);for(int i=0;i<256;i++){uint8_t v=(uint8_t)i,w=0;for(int j=0;j<8;j++)w|=((i>>j)&1)<<(7-j);TIFFReverseBits(&v,1);assert(v==w&&TIFFGetBitRevTable(1)[i]==w&&TIFFGetBitRevTable(0)[i]==i);}
 static const int widths[]={0,1,1,2,4,8,1,1,2,4,8,4,8,4,0,0,8,8,8,0};for(int i=0;i<20;i++)assert(TIFFDataWidth((TIFFDataType)i)==widths[i]);assert(!TIFFDataWidth((TIFFDataType)-1)&&!TIFFIsCODECConfigured(65535)&&TIFFIsCODECConfigured(COMPRESSION_LZW));
}
static void null_test(void){
 uint8_t b[64]={0};uint32_t rgba_buf[16]={0};char message[1024];uint64_t u;uint16_t v;
 assert(!TIFFGetField(NULL,256,&u));assert(!TIFFGetFieldDefaulted(NULL,256,&u));assert(!TIFFSetField(NULL,256,(uint32_t)1));
 assert(!TIFFIsBigEndian(NULL)&&!TIFFIsByteSwapped(NULL)&&!TIFFIsMSB2LSB(NULL)&&!TIFFIsUpSampled(NULL)&&!TIFFLastDirectory(NULL));
 assert(!TIFFNumberOfDirectories(NULL)&&!TIFFNumberOfStrips(NULL)&&!TIFFSetDirectory(NULL,0)&&!TIFFReadDirectory(NULL)&&!TIFFSetSubDirectory(NULL,0));
 assert(!TIFFReadEXIFDirectory(NULL,0)&&!TIFFReadGPSDirectory(NULL,0));
 assert(!TIFFScanlineSize(NULL)&&!TIFFGetStrileOffset(NULL,0)&&TIFFRawStripSize(NULL,0)==-1);assert(TIFFComputeTile(NULL,0,0,0,0)==UINT32_MAX&&TIFFComputeStrip(NULL,0,0)==UINT32_MAX&&!TIFFCheckTile(NULL,0,0,0,0));
 assert(TIFFReadScanline(NULL,b,0,0)==-1&&TIFFReadEncodedStrip(NULL,0,b,1)==-1&&TIFFReadEncodedTile(NULL,0,b,1)==-1&&TIFFReadRawStrip(NULL,0,b,1)==-1&&TIFFReadRawTile(NULL,0,b,1)==-1&&TIFFReadTile(NULL,b,0,0,0,0)==-1);
 assert(!TIFFReadRGBAImage(NULL,1,1,rgba_buf,0)&&!TIFFRGBAImageOK(NULL,message)&&!TIFFRGBAImageBegin(NULL,NULL,0,message));
 assert(!TIFFRGBAImageGet(NULL,rgba_buf,1,1));TIFFRGBAImageEnd(NULL);TIFFClose(NULL);TIFFCleanup(NULL);
 assert(!TIFFFileName(NULL)&&!TIFFSetFileName(NULL,"x")&&TIFFFileno(NULL)==-1&&!TIFFClientdata(NULL)&&TIFFGetMode(NULL)==-1);
 assert(!TIFFGetReadProc(NULL)&&!TIFFGetWriteProc(NULL)&&!TIFFGetSeekProc(NULL)&&!TIFFGetCloseProc(NULL)&&!TIFFGetSizeProc(NULL)&&!TIFFGetMapFileProc(NULL)&&!TIFFGetUnmapFileProc(NULL));
 assert(TIFFCurrentRow(NULL)==UINT32_MAX&&TIFFCurrentStrip(NULL)==UINT32_MAX&&TIFFCurrentTile(NULL)==UINT32_MAX);
 assert(!TIFFFieldTag(NULL)&&!TIFFFieldName(NULL)&&!TIFFFieldDataType(NULL)&&!TIFFFieldPassCount(NULL)&&!TIFFFieldReadCount(NULL)&&!TIFFFieldWriteCount(NULL)&&!TIFFFieldSetGetSize(NULL)&&!TIFFFieldSetGetCountSize(NULL)&&!TIFFFieldIsAnonymous(NULL));
 assert(!TIFFFieldWithTag(NULL,1)&&!TIFFFieldWithName(NULL,"x")&&TIFFMergeFieldInfo(NULL,NULL,0)==-1&&!TIFFGetTagListCount(NULL)&&TIFFGetTagListEntry(NULL,0)==UINT32_MAX);
 assert(!TIFFAccessTagMethods(NULL)&&!TIFFGetClientInfo(NULL,"x"));TIFFSetClientInfo(NULL,NULL,"x");assert(!TIFFReadBufferSetup(NULL,b,1)&&!TIFFUnsetField(NULL,256));TIFFFreeDirectory(NULL);TIFFPrintDirectory(NULL,stderr,0);
 assert(!TIFFReadCustomDirectory(NULL,0,NULL)&&TIFFSetFileno(NULL,1)==-1&&TIFFSetMode(NULL,0)==-1&&!TIFFSetClientdata(NULL,NULL));
 assert(!TIFFWriteCheck(NULL,0,NULL)&&!TIFFWriteDirectory(NULL)&&!TIFFFlush(NULL)&&!TIFFFlushData(NULL)&&!TIFFReadFromUserBuffer(NULL,0,b,1,b,1));
 assert(!VTIFFOpenOptionsSetLimits(NULL,0,0,0));TIFFOpenOptionsSetErrorHandlerExtR(NULL,NULL,NULL);TIFFOpenOptionsSetWarningHandlerExtR(NULL,NULL,NULL);TIFFOpenOptionsSetMaxSingleMemAlloc(NULL,0);TIFFOpenOptionsSetMaxCumulatedMemAlloc(NULL,0);TIFFOpenOptionsSetWarnAboutUnknownTags(NULL,1);
 TIFF fake={0};assert(!TIFFGetField(&fake,258,&v));assert(!TIFFReadBufferSetup(&fake,b,1));
}
static void modes_test(void){
 assert(!TIFFOpen(NULL,"r"));assert(!TIFFOpen("/nonexistent/verified-tiff","r"));assert(!TIFFOpen(gray_path,NULL));
 const char *bad[]={"","a","r+","rx","wx","w8x"};for(size_t i=0;i<sizeof(bad)/sizeof(*bad);i++){assert(vt_mode_flags(bad[i])<0);assert(!new_handle("x",bad[i],NULL));}
 assert(vt_mode_flags("rc")==0&&vt_mode_flags("rcCh")==(VT_CHOP|VT_HEADER)&&vt_mode_flags("rD")==(VT_CHOP|VT_DEFERRED)&&vt_mode_flags("w8b")==(VT_WRITE|VT_BIGTIFF|VT_BIGENDIAN));
 assert(!new_handle(NULL,"r",NULL));char longname[4097];memset(longname,'x',4096);longname[4096]=0;assert(!new_handle(longname,"r",NULL));
 TIFFOpenOptions*o=TIFFOpenOptionsAlloc();assert(o);assert(!VTIFFOpenOptionsSetLimits(o,UINT64_MAX,1,1));assert(!VTIFFOpenOptionsSetLimits(o,1,UINT64_MAX,1));
 TIFFOpenOptionsSetMaxSingleMemAlloc(o,-1);assert(!new_handle("x","r",o));TIFFOpenOptionsFree(o);
 o=TIFFOpenOptionsAlloc();TIFFOpenOptionsSetMaxCumulatedMemAlloc(o,-1);assert(!new_handle("x","r",o));TIFFOpenOptionsFree(o);
 o=TIFFOpenOptionsAlloc();TIFFOpenOptionsSetMaxSingleMemAlloc(o,sizeof(TIFF));assert(!new_handle(longname+1,"r",o));TIFFOpenOptionsFree(o);
 o=TIFFOpenOptionsAlloc();TIFFOpenOptionsSetMaxCumulatedMemAlloc(o,sizeof(TIFF));assert(!new_handle("x","r",o));TIFFOpenOptionsFree(o);
 for(long n=0;n<2;n++){alloc_fail=n;alloc_calls=0;assert(!new_handle("x","r",NULL));clear_fault();}
 alloc_fail=0;alloc_calls=0;assert(!TIFFOpenOptionsAlloc());clear_fault();
 const char *vars[]={"VTIFF_MAX_FILE_BYTES","VTIFF_MAX_BUFFER_BYTES","VTIFF_MAX_WORK","VTIFF_MAX_SINGLE_ALLOC","VTIFF_MAX_TOTAL_ALLOC"};
 const char *badenv[]={"","-1","x","18446744073709551616","9223372036854775808"};
 for(size_t i=0;i<5;i++)for(size_t j=0;j<5;j++){setenv(vars[i],badenv[j],1);o=TIFFOpenOptionsAlloc();assert(o);if(i==2&&j==4)assert(!o->limits.invalid_env);else assert(o->limits.invalid_env);assert(VTIFFOpenOptionsSetLimits(o,1024,1024,1024));TIFFOpenOptionsSetMaxSingleMemAlloc(o,1024);TIFFOpenOptionsSetMaxCumulatedMemAlloc(o,1024);assert(!o->limits.invalid_env);TIFFOpenOptionsFree(o);unsetenv(vars[i]);}
 TIFF*t=gray();assert(TIFFIsMSB2LSB(t));assert(TIFFIsByteSwapped(t)==(TIFFIsBigEndian(t)!=HOST_BIGENDIAN));assert(!TIFFIsUpSampled(t));assert(TIFFCurrentTile(t)==UINT32_MAX);assert(TIFFGetWriteProc(t)==NULL&&TIFFGetCloseProc(t)==NULL&&TIFFGetSizeProc(t)==NULL&&TIFFGetMapFileProc(t)==NULL&&TIFFGetUnmapFileProc(t)==NULL);assert(TIFFFlushData(t));
 const TIFFCodec*c=TIFFFindCODEC(1);assert(c&&c->init(t,1));assert(!c->init(t,5)&&!c->init(NULL,1));assert(!TIFFFindCODEC(65535));TIFFClose(t);
}
static void metadata_test(void){
 TIFF*t=gray();uint32_t tag=64000;
 #define SCALAR(K,T,V,PROM) do {TIFFFieldInfo f={tag,1,1,K,FIELD_CUSTOM,1,0,"Scalar"};assert(!TIFFMergeFieldInfo(t,&f,1));struct{T value;unsigned char tail[16];} out;memset(&out,0xa5,sizeof(out));assert(TIFFSetField(t,tag,(PROM)(V)));assert(TIFFGetField(t,tag,&out.value)&&out.value==(T)(V));for(size_t j=0;j<sizeof(out.tail);j++)assert(out.tail[j]==0xa5);assert(!TIFFGetField(t,tag,(T*)NULL));const TIFFField*q=TIFFFieldWithTag(t,tag);assert(q&&TIFFFieldWriteCount(q)==1);tag++;}while(0)
 SCALAR(TIFF_BYTE,uint8_t,253,int);SCALAR(TIFF_UNDEFINED,uint8_t,252,int);SCALAR(TIFF_SBYTE,int8_t,-127,int);SCALAR(TIFF_SHORT,uint16_t,65534,int);SCALAR(TIFF_SSHORT,int16_t,-32767,int);SCALAR(TIFF_LONG,uint32_t,UINT32_MAX,uint32_t);SCALAR(TIFF_SLONG,int32_t,INT32_MIN,int32_t);SCALAR(TIFF_LONG8,uint64_t,UINT64_MAX,uint64_t);SCALAR(TIFF_SLONG8,int64_t,INT64_MIN,int64_t);SCALAR(TIFF_IFD8,uint64_t,42,uint64_t);SCALAR(TIFF_IFD,uint64_t,42,uint64_t);SCALAR(TIFF_FLOAT,float,1.25,double);SCALAR(TIFF_DOUBLE,double,-2.5,double);
 assert(!TIFFFieldWithName(t,"not-a-field")&&!TIFFFieldWithName(t,NULL));assert(TIFFFieldWithName(t,"Scalar"));assert(!TIFFFindField(t,64000,TIFF_DOUBLE));
 uint16_t p[256];for(int i=0;i<256;i++)p[i]=(uint16_t)(i*257);assert(TIFFSetField(t,TIFFTAG_COLORMAP,p,p,p));uint16_t*a,*b,*c;assert(TIFFGetField(t,TIFFTAG_COLORMAP,&a,&b,&c)&&a[42]==p[42]&&b[42]==p[42]&&c[42]==p[42]);
 assert(!TIFFSetField(t,TIFFTAG_COLORMAP,NULL,p,p));assert(!TIFFSetField(t,TIFFTAG_COLORMAP,p,NULL,p));assert(!TIFFSetField(t,TIFFTAG_COLORMAP,p,p,NULL));
 assert(!TIFFGetField(t,TIFFTAG_COLORMAP,NULL,&b,&c));assert(!TIFFGetField(t,TIFFTAG_COLORMAP,&a,NULL,&c));assert(!TIFFGetField(t,TIFFTAG_COLORMAP,&a,&b,NULL));
 TIFFFieldInfo arr={tag,2,2,TIFF_SHORT,FIELD_CUSTOM,1,0,"Pair"};assert(!TIFFMergeFieldInfo(t,&arr,1));uint16_t pair[]={7,19};assert(TIFFSetField(t,tag,pair));assert(TIFFGetField(t,tag,&a)&&a[0]==7&&a[1]==19);assert(!TIFFSetField(t,tag,NULL));assert(!TIFFGetField(t,tag,NULL));
 FILE*out=tmpfile();assert(out);TIFFPrintDirectory(t,out,TIFFPRINT_STRIPS);assert(ftell(out)>0);fclose(out);TIFFClose(t);
}
static void basic_read_test(void){
 TIFF*t=gray();unsigned char b[16384];uint32_t r[4096];assert(TIFFRasterScanlineSize64(t)==TIFFScanlineSize64(t));assert(TIFFRasterScanlineSize(t)>0&&TIFFVStripSize(t,1)>0&&TIFFVStripSize64(t,1)>0);assert(TIFFRawStripSize(t,0)>0);
 assert(TIFFComputeStrip(t,UINT32_MAX,0)==UINT32_MAX&&TIFFComputeStrip(t,0,1)==0);assert(TIFFReadRawTile(t,0,b,1)==-1);
 assert(TIFFReadEncodedStrip(t,0,b,-2)==-1&&TIFFReadEncodedStrip(t,0,NULL,1)==-1);assert(!TIFFReadRGBAImage(t,1,1,NULL,0));
 TIFFRGBAImage img;char msg[1024];assert(!TIFFRGBAImageBegin(NULL,t,0,msg));assert(TIFFRGBAImageBegin(&img,t,0,msg));img.get=NULL;assert(!TIFFRGBAImageGet(&img,r,1,1));img.get=TIFFRGBAImageGet;img.put.any=(void*)1;assert(!TIFFRGBAImageGet(&img,r,1,1));img.put.any=NULL;img.row_offset=-1;assert(!TIFFRGBAImageGet(&img,r,1,1));img.row_offset=0;img.col_offset=-1;assert(!TIFFRGBAImageGet(&img,r,1,1));img.col_offset=0;assert(!TIFFRGBAImageGet(&img,NULL,1,1));img.row_offset=(int)img.height;assert(!TIFFRGBAImageGet(&img,r,1,1));TIFFRGBAImageEnd(&img);TIFFClose(t);
 t=TIFFOpen(tile_path,"rc");assert(t);assert(TIFFTileRowSize64(t)>0&&TIFFTileRowSize(t)>0&&TIFFVTileSize64(t,1)>0&&TIFFVTileSize(t,1)>0);assert(TIFFReadTile(t,b,0,0,0,0)>0);assert(TIFFCurrentTile(t)==0);assert(TIFFReadTile(t,b,0,0,1,0)==-1);assert(TIFFReadRawStrip(t,0,b,1)==-1);TIFFClose(t);
}

static void bridge_failures_test(void){
 TIFF*t=gray();uint8_t b[64];uint32_t r[64];char msg[1024];uint32_t u=0xa5a5a5a5;uint16_t h=0xa5a5;
 inject("vt_info",0);fault_value=UINT64_MAX;assert(!TIFFIsTiled(t));clear_fault();
 inject("vt_count_dirs",0);assert(!TIFFNumberOfDirectories(t));clear_fault();
 inject("vt_strips",0);fault_value=UINT64_MAX;assert(!TIFFNumberOfStrips(t));clear_fault();
 inject("vt_size",0);fault_value=UINT64_MAX;assert(!TIFFScanlineSize64(t));clear_fault();
 inject("vt_get",-1);assert(!TIFFGetField(t,256,&u)&&u==0xa5a5a5a5);clear_fault();
 assert(!TIFFGetField(t,256,(uint32_t*)NULL));assert(!TIFFGetField(t,258,(uint16_t*)NULL));assert(!TIFFGetField(t,TIFFTAG_JPEGCOLORMODE,(int*)NULL));
 /* A null output leaves every output unchanged. */
 assert(!TIFFGetFieldDefaulted(t,TIFFTAG_YCBCRSUBSAMPLING,&h,NULL)&&h==0xa5a5);assert(!TIFFGetFieldDefaulted(t,TIFFTAG_YCBCRSUBSAMPLING,NULL,&h));
 assert(TIFFRawStripSize(t,UINT32_MAX)==-1);
 inject("vt_select",0);assert(!TIFFSetDirectory(t,0));clear_fault();inject("vt_set_offset",0);assert(!TIFFSetSubDirectory(t,8));clear_fault();
 inject("vt_rgba_ok",0);assert(!TIFFRGBAImageOK(t,msg)&&msg[0]);assert(!TIFFRGBAImageOK(t,NULL));TIFFRGBAImage img;assert(!TIFFRGBAImageBegin(&img,t,0,msg));clear_fault();inject("vt_rgba_begin",0);assert(!TIFFRGBAImageBegin(&img,t,0,msg));clear_fault();
 /* A short read is truncated; a failed one leaves the buffer unchanged. */
 uint8_t whole[64];assert(TIFFReadEncodedStrip(t,0,whole,sizeof(whole))==7*3);memset(b,0xa5,sizeof(b));assert(TIFFReadEncodedStrip(t,0,b,2)==2&&!memcmp(b,whole,2)&&b[2]==0xa5);
 assert(TIFFReadEncodedStrip(t,UINT32_MAX,b,2)==-1&&b[0]==whole[0]&&b[2]==0xa5);assert(!TIFFReadRGBAImage(t,1,1,NULL,0));
 /* A raster taller than the 7x5 image holds it in its last rows. */
 memset(r,0xa5,sizeof(r));assert(TIFFReadRGBAImage(t,1,6,r,0)&&r[0]==0xa5a5a5a5&&r[1]!=0xa5a5a5a5&&r[6]==0xa5a5a5a5);assert(TIFFRGBAImageBegin(&img,t,0,msg));assert(!TIFFRGBAImageGet(&img,NULL,1,1));
 assert(!TIFFReadBufferSetup(t,b,0));assert(!TIFFReadBufferSetup(t,b,-1));
 inject("vt_unset",0);assert(!TIFFUnsetField(t,256));clear_fault();inject("vt_free_directory",0);TIFFFreeDirectory(t);clear_fault();
 inject("vt_clientdata",0);assert(!TIFFSetClientdata(t,(void*)1));clear_fault();
 inject("vt_custom_dir",0);assert(!TIFFReadEXIFDirectory(t,8)&&!TIFFReadGPSDirectory(t,8));clear_fault();
 uint16_t*a=NULL;assert(!TIFFGetField(t,TIFFTAG_STRIPOFFSETS,NULL));assert(!TIFFGetFieldDefaulted(t,TIFFTAG_EXTRASAMPLES,NULL,&a));assert(!TIFFGetFieldDefaulted(t,TIFFTAG_EXTRASAMPLES,&h,NULL));
 assert(TIFFSetField(t,TIFFTAG_SOFTWARE,"a")&&!TIFFGetField(t,TIFFTAG_SOFTWARE,NULL));
 TIFFClose(t);
}
static TIFFDisplay display_fixture(void){TIFFDisplay d={{{1,0,0},{0,1,0},{0,0,1}},100,200,300,255,255,255,0,0,0,1,1,1};return d;}
static void colors_test(void){
 TIFFDisplay d=display_fixture();TIFFCIELabToRGB lab;float white[]={96.425f,100,82.468f};assert(!TIFFCIELabToRGBInit(&lab,&d,white));
 assert(lab.rstep==100.f/CIELABTORGB_TABLE_RANGE&&lab.gstep==200.f/CIELABTORGB_TABLE_RANGE&&lab.bstep==300.f/CIELABTORGB_TABLE_RANGE);
 uint32_t r=0,g=0,b=0;TIFFXYZToRGB(&lab,50,100,150,&r,&g,&b);assert(r>=127&&r<=128&&g>=127&&g<=128&&b>=127&&b<=128);
 float X=71,Y=72,Z=73;TIFFCIELabToXYZ(NULL,0,0,0,&X,&Y,&Z);assert(X==71);TIFFCIELabToXYZ(&lab,0,0,0,NULL,&Y,&Z);TIFFCIELabToXYZ(&lab,0,0,0,&X,NULL,&Z);TIFFCIELabToXYZ(&lab,0,0,0,&X,&Y,NULL);
 int bad[]={-129,128};for(int i=0;i<2;i++){TIFFCIELabToXYZ(&lab,0,bad[i],0,&X,&Y,&Z);TIFFCIELabToXYZ(&lab,0,0,bad[i],&X,&Y,&Z);}TIFFCIELabToXYZ(&lab,256,0,0,&X,&Y,&Z);
 TIFFCIELabToXYZ(&lab,0,-128,127,&X,&Y,&Z);TIFFCIELabToXYZ(&lab,255,127,-128,&X,&Y,&Z);
 TIFFXYZToRGB(NULL,0,0,0,&r,&g,&b);TIFFXYZToRGB(&lab,0,0,0,NULL,&g,&b);TIFFXYZToRGB(&lab,0,0,0,&r,NULL,&b);TIFFXYZToRGB(&lab,0,0,0,&r,&g,NULL);
 float invalid[]={NAN,INFINITY,1e11f,-1e11f};for(int i=0;i<4;i++){TIFFXYZToRGB(&lab,invalid[i],0,0,&r,&g,&b);TIFFXYZToRGB(&lab,0,invalid[i],0,&r,&g,&b);TIFFXYZToRGB(&lab,0,0,invalid[i],&r,&g,&b);}
 lab.range=-1;TIFFXYZToRGB(&lab,0,0,0,&r,&g,&b);lab.range=CIELABTORGB_TABLE_RANGE+1;TIFFXYZToRGB(&lab,0,0,0,&r,&g,&b);lab.range=CIELABTORGB_TABLE_RANGE;TIFFXYZToRGB(&lab,-1,-1,-1,&r,&g,&b);TIFFXYZToRGB(&lab,1000,1000,1000,&r,&g,&b);
 assert(TIFFCIELabToRGBInit(NULL,&d,white)==-1&&TIFFCIELabToRGBInit(&lab,NULL,white)==-1&&TIFFCIELabToRGBInit(&lab,&d,NULL)==-1);
 float badp[]={NAN,-1,0,1e11f};for(int c=0;c<3;c++)for(int j=0;j<4;j++){float w[]={96,100,82};w[c]=badp[j];assert(TIFFCIELabToRGBInit(&lab,&d,w)==-1);}
 for(int k=0;k<9;k++)for(int j=0;j<3;j++){TIFFDisplay v=d;float*ps[]={&v.d_YCR,&v.d_YCG,&v.d_YCB,&v.d_Y0R,&v.d_Y0G,&v.d_Y0B,&v.d_gammaR,&v.d_gammaG,&v.d_gammaB};*ps[k]=(float[]){NAN,-1,1e11f}[j];assert(TIFFCIELabToRGBInit(&lab,&v,white)==-1);}
 for(int k=0;k<6;k++){TIFFDisplay v=d;float*ps[]={&v.d_YCR,&v.d_YCG,&v.d_YCB,&v.d_gammaR,&v.d_gammaG,&v.d_gammaB};*ps[k]=0;assert(TIFFCIELabToRGBInit(&lab,&v,white)==-1);}
 for(int k=0;k<9;k++)for(int j=0;j<2;j++){TIFFDisplay v=d;v.d_mat[k/3][k%3]=j?1e11f:NAN;assert(TIFFCIELabToRGBInit(&lab,&v,white)==-1);}
 for(int k=0;k<3;k++){TIFFDisplay v=d;uint32_t*ps[]={&v.d_Vrwr,&v.d_Vrwg,&v.d_Vrwb};*ps[k]=UINT32_MAX;assert(TIFFCIELabToRGBInit(&lab,&v,white)==-1);v=d;float*fs[]={&v.d_YCR,&v.d_YCG,&v.d_YCB};*fs[k]=nextafterf(0.f,1.f);assert(TIFFCIELabToRGBInit(&lab,&v,white)==-1);}
 size_t n=((sizeof(TIFFYCbCrToRGB)+sizeof(long)-1)/sizeof(long))*sizeof(long)+4*256+2*256*sizeof(int)+3*256*sizeof(int32_t);TIFFYCbCrToRGB*yc=malloc(n);assert(yc);float luma[]={.299f,.587f,.114f},ref[]={0,255,128,255,128,255};
 assert(TIFFYCbCrToRGBInit(NULL,luma,ref)==-1&&TIFFYCbCrToRGBInit(yc,NULL,ref)==-1&&TIFFYCbCrToRGBInit(yc,luma,NULL)==-1);
 for(int k=0;k<3;k++)for(int j=0;j<3;j++){float v[]={.299f,.587f,.114f};v[k]=(float[]){NAN,-1,2}[j];assert(TIFFYCbCrToRGBInit(yc,v,ref)==-1);}
 float zero[]={0,0,0};assert(TIFFYCbCrToRGBInit(yc,zero,ref)==-1);
 for(int k=0;k<6;k++)for(int j=0;j<3;j++){float v[]={0,255,128,255,128,255};v[k]=(float[]){NAN,-1,256}[j];assert(TIFFYCbCrToRGBInit(yc,luma,v)==-1);}
 for(int k=0;k<4;k++){float v[]={128,128,128,128,128,128};if(k==1)for(int i=1;i<6;i+=2)v[i]=128.0001f;if(k==2)for(int i=1;i<6;i+=2)v[i]=127.9999f;if(k==3)for(int i=0;i<6;i++)v[i]=i%2?0:255;assert(!TIFFYCbCrToRGBInit(yc,luma,v));}
 assert(!TIFFYCbCrToRGBInit(yc,luma,ref));TIFFYCbCrtoRGB(yc,300,-1,256,&r,&g,&b);TIFFYCbCrtoRGB(yc,0,256,-1,&r,&g,&b);
 TIFFYCbCrtoRGB(NULL,0,0,0,&r,&g,&b);TIFFYCbCrtoRGB(yc,0,0,0,NULL,&g,&b);TIFFYCbCrtoRGB(yc,0,0,0,&r,NULL,&b);TIFFYCbCrtoRGB(yc,0,0,0,&r,&g,NULL);
 for(int k=0;k<5;k++){TIFFYCbCrToRGB v=*yc;if(k==0)v.Y_tab=NULL;if(k==1)v.Cr_r_tab=NULL;if(k==2)v.Cb_b_tab=NULL;if(k==3)v.Cb_g_tab=NULL;if(k==4)v.Cr_g_tab=NULL;TIFFYCbCrtoRGB(&v,0,0,0,&r,&g,&b);}free(yc);
 float xyz[]={0,0,0};uint8_t rgb[3];XYZtoRGB24(NULL,rgb);XYZtoRGB24(xyz,NULL);for(int k=0;k<3;k++){xyz[k]=NAN;XYZtoRGB24(xyz,rgb);xyz[k]=0;}for(int i=0;i<3;i++){xyz[0]=xyz[1]=xyz[2]=(float[]){-1,.5f,10}[i];XYZtoRGB24(xyz,rgb);}
 double uv;assert(uv_decode(NULL,&uv,0)==-1&&uv_decode(&uv,NULL,0)==-1&&uv_decode(&uv,&uv,-1)==-1);LogLuv24toXYZ(0,NULL);LogLuv32toXYZ(0,NULL);
}
struct memfile{uint8_t data[262144];size_t len,pos;int closed,seek_fail,write_fail;size_t chunk,writes;};
static tmsize_t memread(thandle_t h,void*p,tmsize_t n){struct memfile*m=h;size_t z=(size_t)n;if(z>m->len-m->pos)z=m->len-m->pos;memcpy(p,m->data+m->pos,z);m->pos+=z;return (tmsize_t)z;}
static tmsize_t memwrite(thandle_t h,void*p,tmsize_t n){struct memfile*m=h;if(m->write_fail)return m->write_fail==1?-1:m->write_fail==2?0:n+1;size_t z=(size_t)n;if(m->chunk&&z>m->chunk)z=m->chunk;assert(z<=sizeof(m->data)-m->pos);memcpy(m->data+m->pos,p,z);m->pos+=z;if(m->pos>m->len)m->len=m->pos;m->writes++;return (tmsize_t)z;}
static toff_t memseek(thandle_t h,toff_t o,int w){struct memfile*m=h;if(m->seek_fail)return UINT64_MAX;size_t base=w==SEEK_SET?0:w==SEEK_CUR?m->pos:m->len;if(o>sizeof(m->data)-base)return UINT64_MAX;m->pos=base+(size_t)o;return m->pos;}
static int memclose(thandle_t h){((struct memfile*)h)->closed++;return 0;}
static toff_t memsize(thandle_t h){return ((struct memfile*)h)->len;}
static void load_mem(struct memfile*m){memset(m,0,sizeof(*m));FILE*f=fopen(gray_path,"rb");assert(f);m->len=fread(m->data,1,sizeof(m->data),f);assert(!ferror(f));fclose(f);}
static TIFF *client(struct memfile*m,const char*mode){return TIFFClientOpen("memory",mode,m,memread,memwrite,memseek,memclose,memsize,NULL,NULL);}
static void broken_extender(TIFF*t){TIFFFieldInfo f={64000,1,1,TIFF_SHORT,FIELD_CUSTOM,1,0,NULL};assert(TIFFMergeFieldInfo(t,&f,1)==-1);}
static void callbacks_test(void){
 struct memfile*m=malloc(sizeof(*m));assert(m);load_mem(m);
 assert(!client(m,"bad"));assert(!m->closed);
 assert(!TIFFClientOpen("x","r",m,NULL,memwrite,memseek,memclose,memsize,NULL,NULL));assert(!TIFFClientOpen("x","r",m,memread,memwrite,NULL,memclose,memsize,NULL,NULL));assert(!TIFFClientOpen("x","r",m,memread,memwrite,memseek,memclose,NULL,NULL,NULL));assert(!TIFFClientOpen("x","w",m,memread,NULL,memseek,memclose,memsize,NULL,NULL));assert(!m->closed);
 TIFFOpenOptions*o=TIFFOpenOptionsAlloc();assert(o);o->limits.input=m->len-1;assert(!TIFFClientOpenExt("x","r",m,memread,memwrite,memseek,memclose,memsize,NULL,NULL,o));o->limits.input=SIZE_MAX;size_t saved=m->len;m->len=SIZE_MAX;assert(!TIFFClientOpenExt("x","r",m,memread,memwrite,memseek,memclose,memsize,NULL,NULL,o));m->len=saved;TIFFOpenOptionsFree(o);
 TIFFSetTagExtender(broken_extender);assert(!client(m,"rc"));assert(!m->closed);int fd=open(gray_path,O_RDONLY);assert(fd>=0&&!TIFFFdOpen(fd,"x","w")&&!TIFFFdOpen(fd,"x","q"));assert(!TIFFFdOpen(fd,"x","rc")&&fcntl(fd,F_GETFD)>=0);close(fd);TIFFSetTagExtender(NULL);
 TIFF*t=client(m,"rc");assert(t);TIFFSetTagExtender(broken_extender);assert(!TIFFSetDirectory(t,0));assert(!TIFFSetSubDirectory(t,TIFFCurrentDirOffset(t)));inject("vt_info",0);fault_tag=4;fault_value=8;/* force a next-directory attempt */
 /* next itself is separately injected below; the actual one-directory source fails safely. */
 assert(!TIFFReadDirectory(t));clear_fault();TIFFSetTagExtender(NULL);TIFFClose(t);assert(m->closed==1);
 load_mem(m);t=client(m,"rc");assert(t);assert(TIFFGetWriteProc(t)==memwrite&&TIFFGetCloseProc(t)==memclose&&TIFFGetSizeProc(t)==memsize);t->close=NULL;TIFFClose(t);assert(!m->closed);
 load_mem(m);t=client(m,"w8b");assert(t);uint32_t w=2,h=1;assert(TIFFSetField(t,256,w)&&TIFFSetField(t,257,h)&&TIFFSetField(t,258,8)&&TIFFSetField(t,277,1)&&TIFFSetField(t,262,1));assert(TIFFWriteCheck(t,0,NULL));assert(!TIFFWriteCheck(t,1,NULL));
 m->seek_fail=1;assert(!TIFFWriteDirectory(t));m->seek_fail=0;
 for(int i=1;i<=3;i++){m->write_fail=i;assert(!TIFFWriteDirectory(t));}m->write_fail=0;
 /* A directory larger than one write is written as the callback accepts it. */
 static char description[131072];memset(description,'d',sizeof(description)-1);assert(TIFFSetField(t,TIFFTAG_IMAGEDESCRIPTION,description));
 m->chunk=17000;m->len=0;assert(TIFFWriteDirectory(t)&&m->writes>7);size_t chunked=m->len;uint8_t*whole=malloc(chunked);assert(whole);memcpy(whole,m->data,chunked);
 m->chunk=0;m->len=0;assert(TIFFWriteDirectory(t)&&m->len==chunked&&!memcmp(m->data,whole,chunked));free(whole);
 /* A written directory is written again only once it changes. */
 size_t writes=m->writes;assert(TIFFFlush(t)&&m->writes==writes);assert(TIFFSetField(t,TIFFTAG_SOFTWARE,"x")&&TIFFFlush(t)&&m->writes>writes);
 assert(TIFFSetField(t,TIFFTAG_SOFTWARE,"y"));m->write_fail=1;assert(!TIFFFlush(t));TIFFClose(t);assert(m->closed==1);free(m);
}
static int call_default_set(TIFF*t,uint32_t tag,...){va_list ap;va_start(ap,tag);int r=default_setfield(t,tag,ap);va_end(ap);return r;}
static void fields_failures_test(void){
 TIFF*t=gray();uint16_t*a=NULL,*b=NULL,*c=NULL;uint32_t n32;uint16_t n16;TIFFFieldInfo f={64000,1,1,TIFF_SHORT,FIELD_CUSTOM,1,0,"Test"};
 assert(TIFFMergeFieldInfo(t,NULL,1)==-1&&TIFFMergeFieldInfo(t,&f,4097)==-1&&!TIFFMergeFieldInfo(t,NULL,0));
 for(int i=0;i<4;i++){TIFFFieldInfo bad=f;if(i==0)bad.field_name=NULL;if(i==1)bad.field_type=TIFF_NOTYPE;if(i==2)bad.field_tag=65536;if(i==3)bad.field_readcount=0;assert(TIFFMergeFieldInfo(t,&bad,1)==-1);}
 char big[4097];memset(big,'x',4096);big[4096]=0;f.field_name=big;assert(TIFFMergeFieldInfo(t,&f,1)==-1);f.field_name="Test";
 /* Like libtiff, a handle registers at most 4096 fields. */
 TIFF *full=gray();for(uint32_t i=0;i<4096;i++){TIFFFieldInfo x=f;x.field_tag=1000+i;assert(!TIFFMergeFieldInfo(full,&x,1));}
 assert(TIFFMergeFieldInfo(full,&f,1)==-1);TIFFClose(full);
 TIFFFieldInfo pair[]={f,f};pair[1].field_tag++;pair[1].field_name=NULL;assert(TIFFMergeFieldInfo(t,pair,2)==-1&&!TIFFFindField(t,64000,TIFF_ANY));
 assert(!TIFFMergeFieldInfo(t,&f,1));assert(TIFFFindField(t,64000,TIFF_SHORT));assert(!TIFFFindField(t,65000,TIFF_SHORT)&&!TIFFFindField(t,65536,TIFF_ANY));assert(!TIFFFieldWithName(t,"missing"));
 uint64_t main_dir=TIFFCurrentDirOffset(t);assert(TIFFReadEXIFDirectory(t,main_dir)&&TIFFFieldWithName(t,"ExposureTime"));assert(TIFFReadGPSDirectory(t,main_dir)&&TIFFFieldWithName(t,"VersionID"));assert(TIFFSetDirectory(t,0)&&TIFFFieldWithName(t,"Test"));
 inject("vt_field_named",-1);assert(!TIFFFieldWithName(t,"Test"));clear_fault();
 assert(TIFFFieldWithTag(t,TIFFTAG_LERC_PARAMETERS));assert(TIFFGetTagListEntry(t,-1)==UINT32_MAX);assert(!TIFFSetFileName(t,NULL));assert(!call_default_set(NULL,1,0));
 TIFFTagMethods methods=t->methods;t->methods.vgetfield=NULL;assert(!TIFFGetField(t,256,&n32));t->methods.vsetfield=NULL;assert(!TIFFSetField(t,256,(uint32_t)1));t->methods=methods;
 assert(!TIFFSetField(t,65001,1));assert(!TIFFSetField(t,TIFFTAG_SOFTWARE,NULL));TIFFOpenOptions*small=TIFFOpenOptionsAlloc();assert(small&&VTIFFOpenOptionsSetLimits(small,1<<20,3,1<<20));TIFF*tight=TIFFOpenExt(gray_path,"rc",small);TIFFOpenOptionsFree(small);
 assert(tight&&TIFFSetField(tight,TIFFTAG_SOFTWARE,"ab")&&!TIFFSetField(tight,TIFFTAG_SOFTWARE,"abc"));
 /* The directory's text is an output, within the output limit. */
 FILE*sink=tmpfile();assert(sink);TIFFPrintDirectory(tight,sink,0);assert(ftell(sink)==0);fclose(sink);TIFFClose(tight);
 uint32_t pairs[]={TIFFTAG_PAGENUMBER,TIFFTAG_YCBCRSUBSAMPLING,TIFFTAG_HALFTONEHINTS,TIFFTAG_DOTRANGE};for(int i=0;i<4;i++)assert(TIFFSetField(t,pairs[i],1,1));
 for(int v=0;v<2;v++){TIFFFieldInfo arr={64010+(uint32_t)v,v?-3:-1,v?-3:-1,TIFF_SHORT,FIELD_CUSTOM,1,1,"Array"};assert(!TIFFMergeFieldInfo(t,&arr,1));uint16_t values[]={4,9};assert(v?TIFFSetField(t,arr.field_tag,(uint32_t)2,values):TIFFSetField(t,arr.field_tag,2,values));assert(v?TIFFGetField(t,arr.field_tag,&n32,&a):TIFFGetField(t,arr.field_tag,&n16,&a));assert(!TIFFGetField(t,arr.field_tag,NULL,&a));assert(v?!TIFFGetField(t,arr.field_tag,&n32,NULL):!TIFFGetField(t,arr.field_tag,&n16,NULL));assert(v?TIFFSetField(t,arr.field_tag,(uint32_t)0,NULL):TIFFSetField(t,arr.field_tag,0,NULL));}
 TIFFFieldInfo str={64012,1,1,TIFF_ASCII,FIELD_CUSTOM,1,0,"String"};assert(!TIFFMergeFieldInfo(t,&str,1));assert(TIFFSetField(t,str.field_tag,"a\1\177z"));
 TIFFFieldInfo unsized={64013,-1,-1,TIFF_SHORT,FIELD_CUSTOM,1,0,"Unsized"};assert(!TIFFMergeFieldInfo(t,&unsized,1));assert(!TIFFSetField(t,unsized.field_tag,&n16));
 TIFFFieldInfo asymmetric={64014,1,2,TIFF_SHORT,FIELD_CUSTOM,1,0,"Asymmetric"};assert(!TIFFMergeFieldInfo(t,&asymmetric,1));uint16_t vals[]={2,3};assert(TIFFSetField(t,asymmetric.field_tag,vals));
 for(int i=0;i<2;i++){TIFFFieldInfo rational={64015+(uint32_t)i,1,1,i?TIFF_SRATIONAL:TIFF_RATIONAL,FIELD_CUSTOM,1,0,"Rational"};assert(!TIFFMergeFieldInfo(t,&rational,1));assert(TIFFSetField(t,rational.field_tag,1.25));}
 /* Getters and setters use Rust's registry, not this view of it. */
 TIFFField*node=(TIFFField*)TIFFFindField(t,64016,TIFF_ANY);assert(node);TIFFField saved=*node;node->kind=TIFF_NOTYPE;node->size=0;
 float real=0;assert(TIFFSetField(t,node->tag,1.25)&&TIFFGetField(t,node->tag,&real)&&real==1.25f);*node=saved;
 assert(TIFFGetFieldDefaulted(t,TIFFTAG_TRANSFERFUNCTION,&a));assert(TIFFGetFieldDefaulted(t,TIFFTAG_TRANSFERFUNCTION,&a));assert(!TIFFGetFieldDefaulted(t,TIFFTAG_TRANSFERFUNCTION,NULL));
 assert(!TIFFGetField(t,TIFFTAG_TRANSFERFUNCTION,&a));TIFFFreeDirectory(t);TIFFClose(t);
 t=TIFFOpen(tile_path,"rc");assert(t);assert(TIFFGetFieldDefaulted(t,TIFFTAG_TRANSFERFUNCTION,&a,&b,&c));assert(!TIFFGetFieldDefaulted(t,TIFFTAG_TRANSFERFUNCTION,NULL,&b,&c));assert(!TIFFGetFieldDefaulted(t,TIFFTAG_TRANSFERFUNCTION,&a,NULL,&c));assert(!TIFFGetFieldDefaulted(t,TIFFTAG_TRANSFERFUNCTION,&a,&b,NULL));TIFFClose(t);
}
static void reader_misc_test(void){
 TIFF*t=gray();uint8_t bytes[64];uint32_t pixels[64];uint64_t off=0;char longname[4097];memset(longname,'a',4096);longname[4096]=0;
 assert(!TIFFGetClientInfo(t,NULL));TIFFSetClientInfo(t,NULL,NULL);TIFFSetClientInfo(t,NULL,longname);assert(!TIFFGetClientInfo(t,"missing"));
 TIFFSetClientInfo(t,(void*)1,"first");TIFFSetClientInfo(t,(void*)2,"second");TIFFSetClientInfo(t,(void*)3,"first");assert(TIFFGetClientInfo(t,"first")== (void*)3&&TIFFGetClientInfo(t,"second")==(void*)2&&!TIFFGetClientInfo(t,"missing"));
 alloc_fail=0;alloc_calls=0;assert(!TIFFGetConfiguredCODECs());clear_fault();
 assert(!TIFFUnsetField(t,65534));TIFFPrintDirectory(t,NULL,0);
 assert(!TIFFIsUpSampled(t));
 assert(!TIFFGetField(t,TIFFTAG_SUBIFD,&off,&off));
 /* Like libtiff, a ColorMap needs exactly 1 << BitsPerSample entries per channel. */
 uint16_t *red=NULL,*green=NULL,*blue=NULL;
 TIFF*short_map=TIFFOpen("tests/fixtures/acceptance-deflate.tif","r");assert(short_map);assert(!TIFFGetField(short_map,TIFFTAG_COLORMAP,&red,&green,&blue));TIFFClose(short_map);
 assert(TIFFReadEncodedTile(t,0,bytes,0)==-1);assert(TIFFReadEncodedStrip(t,UINT32_MAX,bytes,0)==-1);
 assert(!TIFFReadRGBAImage(t,UINT32_MAX,UINT32_MAX,pixels,0));
 TIFFRGBAImage img;assert(TIFFRGBAImageOK(t,NULL)&&TIFFRGBAImageBegin(&img,t,0,NULL));TIFF *save=img.tif;img.tif=NULL;assert(!TIFFRGBAImageGet(&img,pixels,1,1));img.tif=save;assert(!TIFFRGBAImageGet(&img,pixels,UINT32_MAX,UINT32_MAX));
 assert(!TIFFReadFromUserBuffer(t,0,bytes,-1,bytes,1));assert(!TIFFReadFromUserBuffer(t,0,bytes,1,bytes,-1));assert(!TIFFReadFromUserBuffer(t,0,NULL,1,bytes,1));assert(!TIFFReadFromUserBuffer(t,0,bytes,1,NULL,1));(void)TIFFReadFromUserBuffer(t,0,NULL,0,NULL,0);
 TIFFClose(t);t=TIFFOpen(tile_path,"rc");assert(t);assert(TIFFIsByteSwapped(t));assert(TIFFComputeStrip(t,0,0)==0);assert(!TIFFCheckTile(t,UINT32_MAX,0,0,0)&&!TIFFCheckTile(t,0,UINT32_MAX,0,0)&&TIFFCheckTile(t,0,0,0,1));assert(TIFFReadEncodedStrip(t,0,bytes,0)==-1);TIFFClose(t);
 assert(!TIFFIsTiled(NULL));
}
static unsigned print_callbacks;
static void printer(TIFF*t,FILE*f,long flags){(void)t;(void)flags;print_callbacks++;fputs("callback\n",f);}
/* libtiff 4.7.1's internal TIFFField and TIFFFieldArray layouts. */
struct vt_import_field {uint32_t tag;short read_count,write_count;TIFFDataType type;uint32_t anonymous;int get_type;unsigned short bit;unsigned char change,pass;char *name;const TIFFFieldArray *children;};
struct vt_import_array {int type;uint32_t allocated,count;const struct vt_import_field *fields;};
static void custom_and_print_test(void){
 TIFF*t=gray();uint64_t main_dir=TIFFCurrentDirOffset(t);struct vt_import_field f={.tag=62000,.read_count=1,.write_count=1,.type=TIFF_SHORT,.get_type=4,.bit=FIELD_CUSTOM,.name="Imported"};struct vt_import_array a={0,0,1,&f};
 a.count=4097;assert(!TIFFReadCustomDirectory(t,8,(const TIFFFieldArray*)&a));a.count=1;a.fields=NULL;assert(!TIFFReadCustomDirectory(t,8,(const TIFFFieldArray*)&a));a.fields=&f;
 for(int i=0;i<5;i++){struct vt_import_field bad=f;if(i==0)bad.name=NULL;if(i==1)bad.tag=65536;if(i>=2)bad.get_type=(int[]){0,51,14}[i-2];a.fields=&bad;assert(!TIFFReadCustomDirectory(t,8,(const TIFFFieldArray*)&a));}a.fields=&f;
 /* A failed import, or a failed read, registers none of the fields. */
 struct vt_import_field two[2]={f,f};two[1].name=NULL;a.fields=two;a.count=2;assert(!TIFFReadCustomDirectory(t,main_dir,(const TIFFFieldArray*)&a));a.fields=&f;a.count=1;
 assert(!TIFFReadCustomDirectory(t,(uint64_t)1<<40,(const TIFFFieldArray*)&a)&&TIFFFieldWithName(t,"ImageWidth"));
 assert(TIFFReadCustomDirectory(t,main_dir,NULL)&&!TIFFFindField(t,62000,TIFF_ANY));assert(TIFFSetDirectory(t,0));
 TIFFFieldInfo other={62001,1,1,TIFF_SHORT,FIELD_CUSTOM,1,0,"Other"};assert(!TIFFMergeFieldInfo(t,&other,1));
 for(int i=0;i<4;i++){f.get_type=(int[]){4,18,30,42}[i];assert(TIFFReadCustomDirectory(t,main_dir,(const TIFFFieldArray*)&a));assert(TIFFFindField(t,62000,TIFF_ANY)&&!TIFFFindField(t,62001,TIFF_ANY));}assert(TIFFSetDirectory(t,0)&&TIFFFindField(t,62001,TIFF_ANY));TIFFClose(t);
 t=gray();assert(TIFFSetField(t,TIFFTAG_SOFTWARE,"a\1\177z"));FILE*out=tmpfile();assert(out);t->methods.printdir=printer;TIFFPrintDirectory(t,out,0);assert(print_callbacks==1);
 TIFFFieldInfo fi={64000,1,1,TIFF_SHORT,FIELD_CUSTOM,1,0,"PrintScalar"};assert(!TIFFMergeFieldInfo(t,&fi,1));assert(TIFFSetField(t,64000,42));
 TIFFPrintDirectory(t,out,0);inject("vt_find_field",-1);assert(!TIFFFindField(t,TIFFTAG_SOFTWARE,TIFF_ANY));clear_fault();assert(TIFFGetTagListCount(t)>=0);assert(TIFFGetTagListEntry(t,100)==UINT32_MAX);
 fclose(out);TIFFClose(t);
 t=TIFFOpen(tile_path,"rc");assert(t);out=tmpfile();assert(out);TIFFPrintDirectory(t,out,TIFFPRINT_STRIPS);fclose(out);TIFFClose(t);
}
static void getter_defaults_test(void){
 TIFF*t=gray();float*p=NULL;double d=0;
 assert(TIFFGetFieldDefaulted(t,TIFFTAG_WHITEPOINT,&p));assert(TIFFGetFieldDefaulted(t,TIFFTAG_YCBCRCOEFFICIENTS,&p));assert(TIFFGetFieldDefaulted(t,TIFFTAG_REFERENCEBLACKWHITE,&p));assert(TIFFGetFieldDefaulted(t,TIFFTAG_STONITS,&d)&&d==1.);
 /* As in libtiff, the strip and tile tags of a pair read one array. */
 uint64_t*strip=NULL,*tile=NULL;assert(TIFFGetField(t,TIFFTAG_STRIPOFFSETS,&strip)&&TIFFGetField(t,TIFFTAG_TILEOFFSETS,&tile)&&strip==tile);
 assert(TIFFGetField(t,TIFFTAG_STRIPBYTECOUNTS,&strip)&&TIFFGetField(t,TIFFTAG_TILEBYTECOUNTS,&tile)&&strip==tile);
 assert(TIFFReadFromUserBuffer(t,0,NULL,0,NULL,0)==0); /* Zero input is rejected, outputs remain untouched. */
 uint8_t bytes[2]={17,41},out[2]={0xa5,0xa5};
 inject("vt_commit",0);/* The real bridge may reject short input before the final copy. */
 (void)TIFFReadFromUserBuffer(t,0,bytes,2,out,2);clear_fault();
 TIFFClose(t);
 t=TIFFOpen("tests/fixtures/jpeg-ycbcr.tif","rc");assert(t&&TIFFUnsetField(t,TIFFTAG_REFERENCEBLACKWHITE));assert(TIFFGetFieldDefaulted(t,TIFFTAG_REFERENCEBLACKWHITE,&p)&&p[2]==128.f);TIFFClose(t);
 t=TIFFOpen("tests/fixtures/alpha-4-1-none.tif","rc");assert(t);TIFFRGBAImage img;assert(TIFFRGBAImageBegin(&img,t,0,NULL)&&img.alpha);TIFFClose(t);
 t=gray();/* A default count of zero is accepted. */
 uint16_t count,*extra;assert(TIFFGetFieldDefaulted(t,TIFFTAG_EXTRASAMPLES,&count,&extra)&&!count);TIFFClose(t);
 float luma[]={.5f,1e-20f,.5f},ref[]={0,255,128,255,128,255};size_t n=((sizeof(TIFFYCbCrToRGB)+sizeof(long)-1)/sizeof(long))*sizeof(long)+4*256+2*256*sizeof(int)+3*256*sizeof(int32_t);TIFFYCbCrToRGB*y=malloc(n);assert(y&&!TIFFYCbCrToRGBInit(y,luma,ref));free(y);
}
static void final_branches_test(const char *planar){
 assert(vt_mode_flags(NULL)<0);TIFF*t=gray();assert(TIFFFlush(t));assert(TIFFCheckTile(t,0,0,0,0)&&TIFFComputeTile(t,0,0,0,0)==0);uint8_t b[16384];assert(TIFFReadRawStrip(t,0,b,0)==0);
 float*p=NULL;assert(!TIFFGetFieldDefaulted(t,TIFFTAG_PRIMARYCHROMATICITIES,&p));
 /* Without the tag, the photometric is guessed from the samples. */
 TIFFRGBAImage img;assert(TIFFRGBAImageBegin(&img,t,0,NULL)&&img.alpha==0);assert(TIFFUnsetField(t,TIFFTAG_PHOTOMETRIC)&&TIFFRGBAImageBegin(&img,t,0,NULL)&&img.photometric==PHOTOMETRIC_MINISBLACK);
 tmsize_t size=TIFFRawStripSize(t,0);assert(size>0);uint8_t*encoded=malloc((size_t)size);assert(encoded);assert(TIFFReadRawStrip(t,0,encoded,size)==size);assert(TIFFReadFromUserBuffer(t,0,encoded,size,NULL,0));b[0]=0xa5;assert(!TIFFReadFromUserBuffer(t,0,encoded,1,b,1)&&b[0]==0xa5);free(encoded);TIFFClose(t);
 t=TIFFOpen("tests/fixtures/jpeg-ycbcr.tif","rc");assert(t);assert(TIFFSetField(t,TIFFTAG_JPEGCOLORMODE,0)&&!TIFFIsUpSampled(t));assert(TIFFSetField(t,TIFFTAG_JPEGCOLORMODE,1)&&TIFFIsUpSampled(t));TIFFClose(t);
 t=TIFFOpen("tests/fixtures/logluv-pages.tif","rc");assert(t);TIFFSetTagExtender(broken_extender);assert(!TIFFReadDirectory(t));TIFFSetTagExtender(NULL);TIFFClose(t);
 t=TIFFOpen(planar,"rc");assert(t);assert(TIFFRasterScanlineSize64(t)==17*3*2);assert(TIFFReadEncodedTile(t,0,NULL,0)==0);assert(!TIFFCheckTile(t,0,0,0,3));assert(TIFFCheckTile(t,0,0,0,1));assert(TIFFComputeTile(t,0,0,0,1)==4);assert(TIFFReadTile(t,b,0,0,0,1)==16*16*2);uint64_t*offsets,*same;assert(TIFFGetField(t,TIFFTAG_TILEOFFSETS,&offsets)&&TIFFGetField(t,TIFFTAG_STRIPOFFSETS,&same)&&same==offsets);assert(TIFFGetField(t,TIFFTAG_TILEBYTECOUNTS,&offsets)&&TIFFGetField(t,TIFFTAG_STRIPBYTECOUNTS,&same)&&same==offsets);
 TIFFClose(t);
 t=TIFFOpen("tests/fixtures/acceptance-planar.tif","rc");assert(t);assert(TIFFComputeStrip(t,0,3)==UINT32_MAX);assert(TIFFComputeStrip(t,0,1)!=UINT32_MAX);TIFFClose(t);
}
int main(int argc,char**argv){assert(argc==4);gray_path=argv[1];tile_path=argv[2];diagnostics_test();utilities_test();null_test();modes_test();metadata_test();basic_read_test();bridge_failures_test();colors_test();callbacks_test();fields_failures_test();reader_misc_test();custom_and_print_test();getter_defaults_test();final_branches_test(argv[3]);assert(owned_count==0);puts("C shim hardening assertions passed");return 0;}
