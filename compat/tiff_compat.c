/* Unverified C ABI/varargs/I/O adapter. No TIFF codec implementation lives
 * here. */
#define _POSIX_C_SOURCE 200809L
#include "bridge.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
_Static_assert(sizeof(void *) == 8 && sizeof(tmsize_t) == 8,
               "Linux LP64 ABI required");
struct TIFFOpenOptions {
  struct vt_options limits;
  TIFFErrorHandlerExtR error, warning;
  void *error_data, *warning_data;
};
struct tiff {
  void *state;
  char *name,*owned_name;
  int fd,owned_fd,mode;
  thandle_t client;
  TIFFReadWriteProc read, write;
  TIFFSeekProc seek;
  TIFFCloseProc close;
  TIFFSizeProc size;
  TIFFMapFileProc map;
  TIFFUnmapFileProc unmap;
  struct TIFFOpenOptions options;
  uint32_t flags;
  TIFFTagMethods methods;
};
static _Thread_local int last_code;
static _Thread_local char last_message[512];
static _Thread_local int reporting;
static void default_error(const char *module, const char *fmt, va_list ap) {
  if (module)
    fprintf(stderr, "%s: ", module);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
}
static _Atomic(TIFFErrorHandler) error_handler = default_error,
                                 warning_handler = default_error;
static _Atomic(TIFFErrorHandlerExt) error_ext, warning_ext;
static void dispatch(TIFF *t, int warning, thandle_t client, const char *module,
                     const char *fmt, va_list ap) {
  va_list cp;
  va_copy(cp, ap);
  vsnprintf(last_message, sizeof(last_message), fmt, cp);
  va_end(cp);
  if (reporting)
    return;
  reporting = 1;
  TIFFErrorHandlerExtR h =
      t ? (warning ? t->options.warning : t->options.error) : NULL;
  int handled = 0;
  if (h) {
    va_copy(cp, ap);
    handled = h(t, warning ? t->options.warning_data : t->options.error_data,
                module, fmt, cp);
    va_end(cp);
  }
  if (!handled) {
    TIFFErrorHandler g =
        atomic_load(warning ? &warning_handler : &error_handler);
    if (g) {
      va_copy(cp, ap);
      g(module, fmt, cp);
      va_end(cp);
    }
    TIFFErrorHandlerExt e = atomic_load(warning ? &warning_ext : &error_ext);
    if (e) {
      va_copy(cp, ap);
      e(client, module, fmt, cp);
      va_end(cp);
    }
  }
  reporting = 0;
}
void TIFFError(const char *m, const char *f, ...) {
  va_list ap;
  va_start(ap, f);
  dispatch(NULL, 0, NULL, m, f, ap);
  va_end(ap);
}
void TIFFWarning(const char *m, const char *f, ...) {
  va_list ap;
  va_start(ap, f);
  dispatch(NULL, 1, NULL, m, f, ap);
  va_end(ap);
}
void TIFFErrorExt(thandle_t c, const char *m, const char *f, ...) {
  va_list ap;
  va_start(ap, f);
  dispatch(NULL, 0, c, m, f, ap);
  va_end(ap);
}
void TIFFWarningExt(thandle_t c, const char *m, const char *f, ...) {
  va_list ap;
  va_start(ap, f);
  dispatch(NULL, 1, c, m, f, ap);
  va_end(ap);
}
void TIFFErrorExtR(TIFF *t, const char *m, const char *f, ...) {
  va_list ap;
  va_start(ap, f);
  dispatch(t, 0, t ? t->client : NULL, m, f, ap);
  va_end(ap);
}
void TIFFWarningExtR(TIFF *t, const char *m, const char *f, ...) {
  va_list ap;
  va_start(ap, f);
  dispatch(t, 1, t ? t->client : NULL, m, f, ap);
  va_end(ap);
}
TIFFErrorHandler TIFFSetErrorHandler(TIFFErrorHandler p) {
  return atomic_exchange(&error_handler, p);
}
TIFFErrorHandler TIFFSetWarningHandler(TIFFErrorHandler p) {
  return atomic_exchange(&warning_handler, p);
}
TIFFErrorHandlerExt TIFFSetErrorHandlerExt(TIFFErrorHandlerExt p) {
  return atomic_exchange(&error_ext, p);
}
TIFFErrorHandlerExt TIFFSetWarningHandlerExt(TIFFErrorHandlerExt p) {
  return atomic_exchange(&warning_ext, p);
}
static void fail(TIFF *t, int code, const char *fn, const char *message) {
  last_code = code;
  TIFFErrorExtR(t, fn, "%s", message);
  errno = code;
}
void vt_not_implemented_for(TIFF *t, const char *name) {
  fail(t, ENOSYS, name, "not implemented by verified-tiff");
}
void vt_not_implemented(const char *name) {
  vt_not_implemented_for(NULL, name);
}
static void bridge_fail(TIFF *t, const char *name) {
  fail(t, vt_error_code(), name, vt_error_message());
}
static int valid(TIFF *t, const char *fn) {
  if (t && t->state)
    return 1;
  fail(t, EINVAL, fn, "invalid/null TIFF handle");
  return 0;
}
int VTIFFLastErrorCode(void) { return last_code; }
const char *VTIFFLastError(void) { return last_message; }
void VTIFFClearError(void) {
  last_code = 0;
  last_message[0] = 0;
}
const char *TIFFGetVersion(void) {
  return "verified-tiff 0.1.0; partial read-only libtiff 4.7.1 C API; adapter "
         "is not formally verified";
}
static struct TIFFOpenOptions defaults(void) {
  struct TIFFOpenOptions o = {0};
  vt_options_default(&o.limits, getenv);
  return o;
}
TIFFOpenOptions *TIFFOpenOptionsAlloc(void) {
  TIFFOpenOptions *o = malloc(sizeof(*o));
  if (o)
    *o = defaults();
  else
    fail(NULL, ENOMEM, __func__, "allocation failed");
  return o;
}
void TIFFOpenOptionsFree(TIFFOpenOptions *o) { free(o); }
int VTIFFOpenOptionsSetLimits(TIFFOpenOptions *o, uint64_t in, uint64_t out,
                              uint64_t work) {
  if (!vt_options_limits(o ? &o->limits : NULL, in, out, work)) {
    bridge_fail(NULL, __func__);
    return 0;
  }
  return 1;
}
void TIFFOpenOptionsSetErrorHandlerExtR(TIFFOpenOptions *o,
                                        TIFFErrorHandlerExtR h, void *u) {
  if (!o) {
    fail(NULL, EINVAL, __func__, "null options");
    return;
  }
  o->error = h;
  o->error_data = u;
}
void TIFFOpenOptionsSetWarningHandlerExtR(TIFFOpenOptions *o,
                                          TIFFErrorHandlerExtR h, void *u) {
  if (!o) {
    fail(NULL, EINVAL, __func__, "null options");
    return;
  }
  o->warning = h;
  o->warning_data = u;
}
/* Limits count requested, live per-handle heap buffers. Zero disables a
 * standard allocation limit; extension input/output/work zero remains zero. */
void TIFFOpenOptionsSetMaxSingleMemAlloc(TIFFOpenOptions *o, tmsize_t n) {
  if (!vt_options_allocation(o ? &o->limits : NULL, 0, n))
    bridge_fail(NULL, __func__);
}
void TIFFOpenOptionsSetMaxCumulatedMemAlloc(TIFFOpenOptions *o, tmsize_t n) {
  if (!vt_options_allocation(o ? &o->limits : NULL, 1, n))
    bridge_fail(NULL, __func__);
}
void TIFFOpenOptionsSetWarnAboutUnknownTags(TIFFOpenOptions *o, int n) {
  if (!o) { fail(NULL, EINVAL, __func__, "null options"); return; }
  o->limits.warn_unknown=n!=0;
}
static void unknown_warnings(TIFF *t);
static int default_getfield(TIFF *,uint32_t,va_list);
static int default_setfield(TIFF *,uint32_t,va_list);
static int initialise_fields(TIFF *t);
static TIFF *new_handle(const char *name, const char *mode,
                        TIFFOpenOptions *options) {
  int64_t flags = vt_mode_flags(mode);
  if (flags < 0) {
    vt_not_implemented("TIFFOpen unsupported read/write mode");
    return NULL;
  }
  struct TIFFOpenOptions limits = options ? *options : defaults();
  size_t namelen = vt_admit(&limits.limits, name, sizeof(TIFF));
  if (!namelen) {
    /* The options' own error handler reports a handle they reject. */
    TIFF temporary = {0}; temporary.options=limits;
    bridge_fail(&temporary, "TIFFOpen"); return NULL;
  }
  TIFF *t = calloc(1, sizeof(*t));
  if (!t) {
    fail(NULL, ENOMEM, "TIFFOpen", "allocation failed");
    return NULL;
  }
  t->name = malloc(namelen);
  if (!t->name) {
    free(t);
    fail(NULL, ENOMEM, "TIFFOpen", "allocation failed");
    return NULL;
  }
  memcpy(t->name, name, namelen);
  t->owned_name=t->name;t->owned_fd=-1;t->mode=(flags&VT_WRITE)?O_RDWR:O_RDONLY;
  t->options = limits;
  t->methods.vgetfield=default_getfield;t->methods.vsetfield=default_setfield;
  t->fd = -1;
  t->flags=(uint32_t)flags;
  return t;
}
static void destroy(TIFF *t) {
  if (!t)
    return;
  vt_close(t->state);
  free(t->owned_name);
  free(t);
}
TIFF *TIFFFdOpenExt(int fd, const char *name, const char *mode,
                    TIFFOpenOptions *options) {
  int64_t flags = vt_mode_flags(mode);
  if (flags < 0 || (flags & VT_WRITE)) {
    vt_not_implemented("TIFFFdOpen unsupported write mode");
    return NULL;
  }
  TIFF *t = new_handle(name, mode, options);
  if (!t)
    return NULL;
  t->state =
      vt_open_fd(fd, &t->options.limits, sizeof(*t)+strlen(t->name)+1, t->flags);
  if (!t->state) {
    bridge_fail(t, __func__);
    destroy(t);
    return NULL;
  }
  t->fd = fd;t->owned_fd=fd;
  if (!initialise_fields(t)) {destroy(t);return NULL;}
  unknown_warnings(t);
  return t;
}
TIFF *TIFFFdOpen(int fd, const char *name, const char *mode) {
  return TIFFFdOpenExt(fd, name, mode, NULL);
}
TIFF *TIFFOpenExt(const char *name, const char *mode,
                  TIFFOpenOptions *options) {
  int64_t flags = vt_mode_flags(mode);
  if (flags < 0 || (flags & VT_WRITE)) {
    vt_not_implemented("TIFFOpen unsupported read/write mode");
    return NULL;
  }
  if (!name) {
    fail(NULL, EINVAL, __func__, "null filename");
    return NULL;
  }
  int fd = open(name, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    fail(NULL, errno, __func__, "cannot open input");
    return NULL;
  }
  TIFF *t = TIFFFdOpenExt(fd, name, mode, options);
  if (!t) {
    int e = errno;
    close(fd);
    errno = e;
  }
  return t;
}
TIFF *TIFFOpen(const char *name, const char *mode) {
  return TIFFOpenExt(name, mode, NULL);
}
TIFF *TIFFClientOpenExt(const char *name, const char *mode, thandle_t client,
                        TIFFReadWriteProc read, TIFFReadWriteProc write,
                        TIFFSeekProc seek, TIFFCloseProc close,
                        TIFFSizeProc size, TIFFMapFileProc map,
                        TIFFUnmapFileProc unmap, TIFFOpenOptions *o) {
  TIFF *t = new_handle(name, mode, o);
  if (!t)
    return NULL;
  t->client = client;
  t->read = read;
  t->write = write;
  t->seek = seek;
  t->close = close;
  t->size = size;
  t->map = map;
  t->unmap = unmap;
  if (!read || !seek || !size || ((t->flags&VT_WRITE)&&!write)) {
    fail(t, EINVAL, __func__, "read/seek/size callbacks required");
    destroy(t);
    return NULL;
  }
  /* Like libtiff, a writer never asks for the size of its new output. */
  uint64_t n = t->flags & VT_WRITE ? 0 : size(client);
  t->state=vt_open_client(client, read, write, seek, n, &t->options.limits,
      sizeof(*t)+strlen(t->name)+1, t->flags);
  if (!t->state) {
    bridge_fail(t, __func__);
    destroy(t);
    return NULL;
  }
  if (!initialise_fields(t)) {destroy(t);return NULL;}
  unknown_warnings(t);
  return t;
}
TIFF *TIFFClientOpen(const char *n, const char *m, thandle_t c,
                     TIFFReadWriteProc r, TIFFReadWriteProc w, TIFFSeekProc s,
                     TIFFCloseProc cl, TIFFSizeProc sz, TIFFMapFileProc mp,
                     TIFFUnmapFileProc u) {
  return TIFFClientOpenExt(n, m, c, r, w, s, cl, sz, mp, u, NULL);
}
void TIFFCleanup(TIFF *t) { destroy(t); }
void TIFFClose(TIFF *t) {
  if (!t)
    return;
  if (!vt_flush(t->state))
    bridge_fail(t, __func__);
  int fd = t->owned_fd;
  TIFFCloseProc close_cb = t->close;
  thandle_t c = t->client;
  destroy(t);
  if (close_cb)
    close_cb(c);
  else if (fd >= 0)
    close(fd);
}
const char *TIFFFileName(TIFF *t) { return t ? t->name : NULL; }
const char *TIFFSetFileName(TIFF *t, const char *n) {
  if(!valid(t,__func__)||!n)return NULL;
  const char *old=t->name;t->name=(char*)n;return old;
}
int TIFFFileno(TIFF *t) { return t ? t->fd : -1; }
thandle_t TIFFClientdata(TIFF *t) { return t ? t->client : NULL; }
int TIFFGetMode(TIFF *t) { return valid(t, __func__) ? t->mode : -1; }
TIFFReadWriteProc TIFFGetReadProc(TIFF *t) { return t ? t->read : NULL; }
TIFFReadWriteProc TIFFGetWriteProc(TIFF *t) { return t ? t->write : NULL; }
TIFFSeekProc TIFFGetSeekProc(TIFF *t) { return t ? t->seek : NULL; }
TIFFCloseProc TIFFGetCloseProc(TIFF *t) { return t ? t->close : NULL; }
TIFFSizeProc TIFFGetSizeProc(TIFF *t) { return t ? t->size : NULL; }
TIFFMapFileProc TIFFGetMapFileProc(TIFF *t) { return t ? t->map : NULL; }
TIFFUnmapFileProc TIFFGetUnmapFileProc(TIFF *t) { return t ? t->unmap : NULL; }
static uint64_t info(TIFF *t, unsigned key) {
  if (!valid(t, "TIFF metadata"))
    return 0;
  uint64_t v = vt_info(t->state, key);
  if (v == UINT64_MAX) {
    bridge_fail(t, "TIFF metadata");
    return 0;
  }
  return v;
}
int TIFFIsTiled(TIFF *t) { return (int)info(t, 5); }
int TIFFIsBigEndian(TIFF *t) { return valid(t, __func__) ? !info(t, 6) : 0; }
int TIFFIsByteSwapped(TIFF *t) {
  return valid(t, __func__) ? (TIFFIsBigEndian(t) != HOST_BIGENDIAN) : 0;
}
int TIFFIsMSB2LSB(TIFF *t) { return valid(t, __func__) ? 1 : 0; }
int TIFFIsBigTIFF(TIFF *t) { return (int)info(t, 14); }
int TIFFIsUpSampled(TIFF *t) {
  return valid(t, __func__) && vt_upsampled(t->state);
}
tdir_t TIFFCurrentDirectory(TIFF *t) { return (tdir_t)info(t, 2); }
uint64_t TIFFCurrentDirOffset(TIFF *t) { return info(t, 3); }
uint32_t TIFFCurrentRow(TIFF *t) { return t ? vt_position(t->state, VT_ROW) : UINT32_MAX; }
uint32_t TIFFCurrentStrip(TIFF *t) { return t ? vt_position(t->state, VT_STRIP) : UINT32_MAX; }
uint32_t TIFFCurrentTile(TIFF *t) { return t ? vt_position(t->state, VT_TILE) : UINT32_MAX; }
int TIFFLastDirectory(TIFF *t) { return valid(t, __func__) && info(t, 4) == 0; }
tdir_t TIFFNumberOfDirectories(TIFF *t) {
  if (!valid(t, __func__))
    return 0;
  uint32_t n = vt_count_dirs(t->state);
  if (!n)
    bridge_fail(t, __func__);
  return n;
}
int TIFFSetDirectory(TIFF *t, tdir_t d) {
  if (!valid(t, __func__))
    return 0;
  if (!vt_select(t->state, d)) {
    bridge_fail(t, __func__);
    return 0;
  }
  if (!initialise_fields(t)) return 0;
  unknown_warnings(t);
  return 1;
}
int TIFFReadDirectory(TIFF *t) {
  if (!valid(t, __func__) || TIFFLastDirectory(t))
    return 0;
  if (!vt_next(t->state)) { bridge_fail(t, __func__); return 0; }
  if (!initialise_fields(t)) return 0;
  unknown_warnings(t);
  return 1;
}
int TIFFSetSubDirectory(TIFF *t, uint64_t off) {
  if (!valid(t, __func__))
    return 0;
  if (!vt_set_offset(t->state, off)) {
    bridge_fail(t, __func__);
    return 0;
  }
  if (!initialise_fields(t)) return 0;
  unknown_warnings(t);
  return 1;
}
/* TIFFGetField writes the values compat/src/get.rs plans for the tag through
   the caller's pointers. Each pointer is read with the C type of its value;
   values of one width are stored alike. */
static int getfield(TIFF *t, uint32_t tag, va_list ap, int def) {
  if (!valid(t, "TIFFGetField"))
    return 0;
  struct vt_plan plan;
  int r = vt_get(t->state, tag, def, &plan);
  if (r < 0) {
    bridge_fail(t, "TIFFGetField");
    return 0;
  }
  if (!r)
    return 0;
  void *out[3];
  for (size_t i = 0; i < plan.count; i++)
    switch (plan.value[i].type) {
    case VT_U8: out[i] = va_arg(ap, uint8_t *); break;
    case VT_I8: out[i] = va_arg(ap, int8_t *); break;
    case VT_U16: out[i] = va_arg(ap, uint16_t *); break;
    case VT_I16: out[i] = va_arg(ap, int16_t *); break;
    case VT_U32: out[i] = va_arg(ap, uint32_t *); break;
    case VT_I32: out[i] = va_arg(ap, int32_t *); break;
    case VT_U64: out[i] = va_arg(ap, uint64_t *); break;
    case VT_I64: out[i] = va_arg(ap, int64_t *); break;
    case VT_FLOAT: out[i] = va_arg(ap, float *); break;
    case VT_DOUBLE: out[i] = va_arg(ap, double *); break;
    default: out[i] = va_arg(ap, void **); break;
    }
  for (size_t i = 0; i < plan.count; i++)
    if (!out[i]) {
      fail(t, EINVAL, "TIFFGetField", "null output");
      return 0;
    }
  for (size_t i = 0; i < plan.count; i++) {
    const struct vt_value *v = &plan.value[i];
    uint8_t b8 = (uint8_t)v->bits;
    uint16_t b16 = (uint16_t)v->bits;
    uint32_t b32 = (uint32_t)v->bits;
    switch (v->type) {
    case VT_U8: case VT_I8: memcpy(out[i], &b8, sizeof b8); break;
    case VT_U16: case VT_I16: memcpy(out[i], &b16, sizeof b16); break;
    case VT_U32: case VT_I32: case VT_FLOAT: memcpy(out[i], &b32, sizeof b32); break;
    case VT_U64: case VT_I64: case VT_DOUBLE: memcpy(out[i], &v->bits, sizeof v->bits); break;
    default: *(void **)out[i] = (void *)v->pointer; break;
    }
  }
  return 1;
}
static int default_getfield(TIFF *t,uint32_t k,va_list ap){return getfield(t,k,ap,0);}
int TIFFVGetField(TIFF *t, uint32_t k, va_list ap) {
  if(!valid(t,__func__)||!t->methods.vgetfield)return 0;
  return t->methods.vgetfield(t,k,ap);
}
int TIFFVGetFieldDefaulted(TIFF *t, uint32_t k, va_list ap) {
  va_list cp;va_copy(cp,ap);int r=TIFFVGetField(t,k,cp);va_end(cp);
  return r?r:getfield(t,k,ap,1);
}
int TIFFGetField(TIFF *t, uint32_t k, ...) {
  va_list ap;
  va_start(ap, k);
  int r = TIFFVGetField(t, k, ap);
  va_end(ap);
  return r;
}
int TIFFGetFieldDefaulted(TIFF *t, uint32_t k, ...) {
  va_list ap;
  va_start(ap, k);
  int r = TIFFVGetFieldDefaulted(t, k, ap);
  va_end(ap);
  return r;
}
/* TIFFSetField reads the arguments compat/src/set.rs plans for the tag, each
   with its C type, and Rust stores the field from them. */
static int default_setfield(TIFF *t, uint32_t tag, va_list ap) {
  if (!valid(t, "TIFFSetField"))
    return 0;
  struct vt_plan plan;
  if (!vt_set_plan(t->state, tag, &plan)) {
    bridge_fail(t, "TIFFSetField");
    return 0;
  }
  for (size_t i = 0; i < plan.count; i++) {
    struct vt_value *v = &plan.value[i];
    switch (v->type) {
    case VT_I32: v->bits = (uint64_t)(int64_t)va_arg(ap, int); break;
    case VT_U32: v->bits = va_arg(ap, uint32_t); break;
    case VT_U64: v->bits = va_arg(ap, uint64_t); break;
    case VT_I64: v->bits = (uint64_t)va_arg(ap, int64_t); break;
    case VT_DOUBLE: {
      double d = va_arg(ap, double);
      memcpy(&v->bits, &d, sizeof d);
      break;
    }
    default: v->pointer = va_arg(ap, const void *); break;
    }
  }
  if (!vt_set(t->state, tag, &plan)) {
    bridge_fail(t, "TIFFSetField");
    return 0;
  }
  return 1;
}
int TIFFVSetField(TIFF *t,uint32_t tag,va_list ap){
  if(!valid(t,__func__)||!t->methods.vsetfield)return 0;
  return t->methods.vsetfield(t,tag,ap);
}
int TIFFSetField(TIFF *t, uint32_t tag, ...) {
  va_list ap;
  va_start(ap, tag);
  int r = TIFFVSetField(t, tag, ap);
  va_end(ap);
  return r;
}
static uint64_t size64(TIFF *t, unsigned kind, uint32_t rows) {
  if (!valid(t, "TIFF size"))
    return 0;
  int64_t n = vt_size(t->state, kind, rows);
  if (n < 0) {
    bridge_fail(t, "TIFF size");
    return 0;
  }
  return (uint64_t)n;
}
uint64_t TIFFScanlineSize64(TIFF *t) { return size64(t, 0, 1); }
tmsize_t TIFFScanlineSize(TIFF *t) { return (tmsize_t)TIFFScanlineSize64(t); }
uint64_t TIFFRasterScanlineSize64(TIFF *t) { return size64(t, 4, 1); }
tmsize_t TIFFRasterScanlineSize(TIFF *t) {
  return (tmsize_t)TIFFRasterScanlineSize64(t);
}
uint64_t TIFFStripSize64(TIFF *t) { return size64(t, 1, UINT32_MAX); }
tmsize_t TIFFStripSize(TIFF *t) { return (tmsize_t)TIFFStripSize64(t); }
uint64_t TIFFVStripSize64(TIFF *t, uint32_t n) { return size64(t, 1, n); }
tmsize_t TIFFVStripSize(TIFF *t, uint32_t n) {
  return (tmsize_t)TIFFVStripSize64(t, n);
}
uint64_t TIFFTileRowSize64(TIFF *t) { return size64(t, 2, 1); }
tmsize_t TIFFTileRowSize(TIFF *t) { return (tmsize_t)TIFFTileRowSize64(t); }
uint64_t TIFFTileSize64(TIFF *t) { return size64(t, 3, UINT32_MAX); }
tmsize_t TIFFTileSize(TIFF *t) { return (tmsize_t)TIFFTileSize64(t); }
uint64_t TIFFVTileSize64(TIFF *t, uint32_t n) { return size64(t, 3, n); }
tmsize_t TIFFVTileSize(TIFF *t, uint32_t n) {
  return (tmsize_t)TIFFVTileSize64(t, n);
}
uint32_t TIFFNumberOfStrips(TIFF *t) {
  if (!valid(t, __func__))
    return 0;
  int64_t n = vt_strips(t->state);
  if (n < 0) {
    bridge_fail(t, __func__);
    return 0;
  }
  return (uint32_t)n;
}
uint32_t TIFFNumberOfTiles(TIFF *t) { return (uint32_t)info(t, 7); }
static uint64_t strile(TIFF *t, uint32_t index, int counts, int *error) {
  if (error)
    *error = 1;
  if (!valid(t, "TIFFGetStrile"))
    return 0;
  uint64_t v=0;
  if(!vt_strile(t->state,index,counts,&v)){bridge_fail(t,"TIFFGetStrile");return 0;}
  if(error)*error=0;
  return v;
}
uint64_t TIFFGetStrileOffsetWithErr(TIFF *t, uint32_t k, int *e) {
  return strile(t, k, 0, e);
}
uint64_t TIFFGetStrileByteCountWithErr(TIFF *t, uint32_t k, int *e) {
  return strile(t, k, 1, e);
}
uint64_t TIFFGetStrileOffset(TIFF *t, uint32_t k) {
  return strile(t, k, 0, NULL);
}
uint64_t TIFFGetStrileByteCount(TIFF *t, uint32_t k) {
  return strile(t, k, 1, NULL);
}
uint64_t TIFFRawStripSize64(TIFF *t, uint32_t k) {
  return TIFFGetStrileByteCount(t, k);
}
tmsize_t TIFFRawStripSize(TIFF *t, uint32_t k) {
  if (!valid(t, __func__))
    return -1;
  int64_t n = vt_raw_size(t->state, k);
  if (n < 0)
    bridge_fail(t, __func__);
  return n;
}
int TIFFCheckTile(TIFF *t, uint32_t x, uint32_t y, uint32_t z, uint16_t s) {
  if (!valid(t, __func__))
    return 0;
  if (!vt_check_tile(t->state, x, y, z, s)) {
    bridge_fail(t, __func__);
    return 0;
  }
  return 1;
}
uint32_t TIFFComputeTile(TIFF *t, uint32_t x, uint32_t y, uint32_t z,
                         uint16_t s) {
  uint32_t k = 0;
  if (!valid(t, __func__))
    return UINT32_MAX;
  if (!vt_compute_tile(t->state, x, y, z, s, &k)) {
    bridge_fail(t, __func__);
    return UINT32_MAX;
  }
  return k;
}
uint32_t TIFFComputeStrip(TIFF *t, uint32_t row, uint16_t s) {
  uint32_t k = 0;
  if (!valid(t, __func__))
    return UINT32_MAX;
  if (!vt_compute_strip(t->state, row, s, &k)) {
    bridge_fail(t, __func__);
    return UINT32_MAX;
  }
  return k;
}
/* Reads copy into the caller's buffer only when they succeed. */
static tmsize_t read_into(TIFF *t, unsigned op, uint32_t a, uint32_t b,
                          void *buf, tmsize_t size, const char *fn) {
  if (!valid(t, fn))
    return -1;
  int64_t n = vt_read_into(t->state, op, a, b, buf, size);
  if (n < 0)
    bridge_fail(t, fn);
  return n;
}
int TIFFReadScanline(TIFF *t, void *buf, uint32_t row, uint16_t sample) {
  return read_into(t, VT_READ_SCANLINE, row, sample, buf, -1, __func__) < 0 ? -1 : 1;
}
tmsize_t TIFFReadEncodedStrip(TIFF *t, uint32_t k, void *b, tmsize_t n) {
  return read_into(t, VT_READ_STRIP, k, 0, b, n, __func__);
}
tmsize_t TIFFReadEncodedTile(TIFF *t, uint32_t k, void *b, tmsize_t n) {
  return read_into(t, VT_READ_TILE, k, 0, b, n, __func__);
}
tmsize_t TIFFReadRawStrip(TIFF *t, uint32_t k, void *b, tmsize_t n) {
  return read_into(t, VT_READ_RAW_STRIP, k, 0, b, n, __func__);
}
tmsize_t TIFFReadRawTile(TIFF *t, uint32_t k, void *b, tmsize_t n) {
  return read_into(t, VT_READ_RAW_TILE, k, 0, b, n, __func__);
}
tmsize_t TIFFReadTile(TIFF *t, void *b, uint32_t x, uint32_t y, uint32_t z,
                      uint16_t s) {
  uint32_t k = TIFFComputeTile(t, x, y, z, s);
  return k == UINT32_MAX ? -1 : TIFFReadEncodedTile(t, k, b, -1);
}
static int rgba(TIFF *t, unsigned op, uint32_t a, uint32_t b,
                uint32_t orientation, uint32_t *out, const char *fn) {
  if (!valid(t, fn))
    return 0;
  if (!vt_rgba_into(t->state, op, a, b, orientation, out)) {
    bridge_fail(t, fn);
    return 0;
  }
  return 1;
}
int TIFFReadRGBAImageOriented(TIFF *t, uint32_t w, uint32_t h, uint32_t *p,
                              int o, int stop) {
  (void)stop;
  return rgba(t, VT_RGBA_IMAGE, w, h, (uint32_t)o, p, __func__);
}
int TIFFReadRGBAImage(TIFF *t, uint32_t w, uint32_t h, uint32_t *p, int stop) {
  return TIFFReadRGBAImageOriented(t, w, h, p, ORIENTATION_BOTLEFT, stop);
}
int TIFFReadRGBAStripExt(TIFF *t, uint32_t row, uint32_t *p, int stop) {
  (void)stop;
  return rgba(t, VT_RGBA_STRIP, row, 0, ORIENTATION_BOTLEFT, p, __func__);
}
int TIFFReadRGBAStrip(TIFF *t, uint32_t row, uint32_t *p) {
  return TIFFReadRGBAStripExt(t, row, p, 0);
}
int TIFFReadRGBATileExt(TIFF *t, uint32_t x, uint32_t y, uint32_t *p,
                        int stop) {
  (void)stop;
  return rgba(t, VT_RGBA_TILE, x, y, ORIENTATION_BOTLEFT, p, __func__);
}
int TIFFReadRGBATile(TIFF *t, uint32_t x, uint32_t y, uint32_t *p) {
  return TIFFReadRGBATileExt(t, x, y, p, 0);
}
int TIFFRGBAImageOK(TIFF *t, char message[1024]) {
  if (!valid(t, __func__))
    return 0;
  if (!vt_rgba_ok(t->state)) {
    bridge_fail(t, __func__);
    if (message)
      snprintf(message, 1024, "%s", VTIFFLastError());
    return 0;
  }
  if (message)
    message[0] = 0;
  return 1;
}
int TIFFRGBAImageBegin(TIFFRGBAImage *img, TIFF *t, int stop,
                       char message[1024]) {
  if (!img) {
    fail(t, EINVAL, __func__, "null RGBA state");
    return 0;
  }
  memset(img, 0, sizeof(*img));
  if (!TIFFRGBAImageOK(t, message))
    return 0;
  struct vt_rgba r;
  if (!vt_rgba_begin(t->state, &r)) {
    bridge_fail(t, __func__);
    return 0;
  }
  img->tif = t;
  img->stoponerr = stop;
  img->isContig = r.contig;
  img->alpha = r.alpha;
  img->width = r.width;
  img->height = r.height;
  img->bitspersample = r.bits;
  img->samplesperpixel = r.samples;
  img->orientation = r.orientation;
  img->req_orientation = ORIENTATION_BOTLEFT;
  img->photometric = r.photometric;
  img->redcmap = (uint16_t *)r.colormap[0];
  img->greencmap = (uint16_t *)r.colormap[1];
  img->bluecmap = (uint16_t *)r.colormap[2];
  img->get = TIFFRGBAImageGet;
  return 1;
}
int TIFFRGBAImageGet(TIFFRGBAImage *img, uint32_t *out, uint32_t w,
                     uint32_t h) {
  if (!img) {
    fail(NULL, EINVAL, __func__, "null RGBA state");
    return 0;
  }
  if (img->put.any || img->get != TIFFRGBAImageGet) {
    vt_not_implemented_for(img->tif, "custom TIFFRGBAImage callbacks");
    return 0;
  }
  if (!valid(img->tif, __func__))
    return 0;
  if (!vt_region_into(img->tif->state, img->col_offset, img->row_offset,
                      img->height, w, h, img->orientation,
                      img->req_orientation, out)) {
    bridge_fail(img->tif, __func__);
    return 0;
  }
  return 1;
}
void TIFFRGBAImageEnd(TIFFRGBAImage *img) {
  if (img)
    memset(img, 0, sizeof(*img));
}
/* Standard allocation/byte utilities obey their usual C pointer contracts. */
void *_TIFFmalloc(tmsize_t n) {
  if (n < 0) {
    fail(NULL, EINVAL, __func__, "negative size");
    return NULL;
  }
  void *p = malloc(n ? (size_t)n : 1);
  if (!p)
    fail(NULL, ENOMEM, __func__, "allocation failed");
  return p;
}
void *_TIFFcalloc(tmsize_t n, tmsize_t s) {
  if (n < 0 || s < 0 || (s && n > PTRDIFF_MAX / s)) {
    fail(NULL, EINVAL, __func__, "invalid/overflowing size");
    return NULL;
  }
  size_t bytes=(size_t)n*(size_t)s;
  void *p = calloc(1,bytes ? bytes : 1);
  if (!p)
    fail(NULL, ENOMEM, __func__, "allocation failed");
  return p;
}
void *_TIFFrealloc(void *p, tmsize_t n) {
  if (n < 0) {
    fail(NULL, EINVAL, __func__, "negative size");
    return NULL;
  }
  void *q = realloc(p, n ? (size_t)n : 1);
  if (!q)
    fail(NULL, ENOMEM, __func__, "allocation failed");
  return q;
}
void _TIFFfree(void *p) { free(p); }
void _TIFFmemset(void *p, int v, tmsize_t n) {
  if (n < 0 || (!p && n)) {
    fail(NULL, EINVAL, __func__, "invalid buffer/length");
    return;
  }
  if (n)
    memset(p, v, (size_t)n);
}
void _TIFFmemcpy(void *d, const void *s, tmsize_t n) {
  if (n < 0 || ((!d || !s) && n)) {
    fail(NULL, EINVAL, __func__, "invalid buffer/length");
    return;
  }
  if (n)
    memcpy(d, s, (size_t)n);
}
int _TIFFmemcmp(const void *a, const void *b, tmsize_t n) {
  if (n < 0 || ((!a || !b) && n)) {
    fail(NULL, EINVAL, __func__, "invalid buffer/length");
    return 0;
  }
  return n ? memcmp(a, b, (size_t)n) : 0;
}
static void swap(void *p, tmsize_t n, size_t width) {
  if (!vt_swab(p, n, width))
    bridge_fail(NULL, "TIFFSwab");
}
#define SWAB(Name, Type, Width)                                                \
  void TIFFSwab##Name(Type *p) { swap(p, 1, Width); }                          \
  void TIFFSwabArrayOf##Name(Type *p, tmsize_t n) { swap(p, n, Width); }
SWAB(Short, uint16_t, 2)
SWAB(Long, uint32_t, 4)
SWAB(Long8, uint64_t, 8)
SWAB(Float, float, 4)
SWAB(Double, double, 8)
void TIFFSwabArrayOfTriples(uint8_t *p, tmsize_t n) { swap(p, n, 3); }
void TIFFReverseBits(uint8_t *p, tmsize_t n) {
  if (!vt_reverse_bits(p, n))
    bridge_fail(NULL, __func__);
}
const unsigned char *TIFFGetBitRevTable(int reversed) {
  return vt_bit_table(reversed);
}
int TIFFDataWidth(TIFFDataType type) { return vt_data_width(type); }
/* The configured codecs are those of reader_api.inc's table. */
int TIFFIsCODECConfigured(uint16_t c) { return TIFFFindCODEC(c) != NULL; }

#include "fields.inc"

#include "writer.inc"

#include "reader_api.inc"
#include "color_api.inc"

#include "logluv_api.inc"
