#ifndef VT_BRIDGE_H
#define VT_BRIDGE_H
#include "include/verified_tiff.h"
#include <stddef.h>
#include <stdint.h>
/* The C view of a field of compat/src/fields.rs, which checks this layout. */
struct _TIFFField {
  uint32_t tag;
  TIFFDataType type;
  int read_count, write_count, pass_count, size, count_size, custom;
  const char *name;
  TIFFDataType kind;
  int array, anonymous;
};
_Static_assert(sizeof(TIFFField) == 56 && offsetof(TIFFField, name) == 32 &&
               offsetof(TIFFField, kind) == 40 && offsetof(TIFFField, anonymous) == 48,
               "field view layout");
/* compat/src/fields.rs reads these caller structures with the same layout. */
_Static_assert(sizeof(TIFFFieldInfo) == 24 && offsetof(TIFFFieldInfo, field_name) == 16,
               "TIFFFieldInfo layout");
void *vt_open_fd(int, size_t, size_t, size_t, size_t, size_t, size_t, uint32_t);
void *vt_open_client(thandle_t, TIFFReadWriteProc, TIFFSeekProc, uint64_t, size_t, size_t, size_t, size_t, size_t, size_t, uint32_t);
int vt_next(void *);
int vt_clientdata(void *,thandle_t);
const uint8_t *vt_read_size(void *,uint32_t,uint32_t,uint32_t,size_t,size_t *);
int vt_read_buffer(void *,uint8_t *,size_t);
int vt_unset(void *,uint32_t);
int vt_free_directory(void *);
int vt_strile(void *,uint32_t,int,uint64_t *);
const uint8_t *vt_field(void *, uint32_t, uint16_t *, size_t *);
int vt_real(void *, uint32_t, float *);
int vt_pair(void *, uint32_t, int, uint16_t *);
int vt_channels(void *, uint32_t, int, uint16_t, uint16_t, uint16_t, const uint16_t **, size_t *);
uint32_t vt_tag(void *, size_t);
const uint8_t *vt_region(void *,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,size_t *);
void vt_close(void *);
int vt_put_real(void *,uint32_t,uint16_t,uint16_t,size_t,const void *);
int vt_put(void *, uint32_t, uint16_t, size_t, const uint8_t *);
const uint8_t *vt_user_buffer(void *,uint32_t,const uint8_t *,size_t,size_t);
int vt_write_check(void *);
const uint8_t *vt_directory_bytes(void *,size_t *);
int vt_typed(void *, uint32_t, uint16_t, const uint8_t **, size_t *);
int vt_custom_dir(void *, uint64_t, uint32_t);
int vt_mem_claim(void *, size_t);
void vt_mem_release(void *, size_t);
int vt_error_code(void);
const char *vt_error_message(void);
int vt_select(void *, uint32_t);
int vt_scalar(void *, uint32_t, int, uint64_t *);
uint64_t vt_info(void *, uint32_t);
const void *vt_array(void *, uint32_t, size_t *);
uint32_t vt_count_dirs(void *);
int vt_set_offset(void *, uint64_t);
int64_t vt_size(void *, uint32_t, uint32_t);
int vt_mode(void *, uint32_t, int);
int vt_rgba_ok(void *);
const uint8_t *vt_read(void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                       size_t *);
void vt_not_implemented(const char *);
void vt_not_implemented_for(TIFF *, const char *);
int vt_merge_fields(void *, const TIFFFieldInfo *, uint32_t);
int vt_read_custom_directory(void *, uint64_t, const TIFFFieldArray *);
void vt_fields_begin(void *);
int vt_fields_failed(void *);
int vt_find_field(void *, uint32_t, TIFFDataType, int, const TIFFField **);
int vt_field_named(void *, const char *, const TIFFField **);
uint32_t vt_custom_tags(void *);
uint32_t vt_custom_tag(void *, uint32_t);
uint32_t vt_unknown_tag(void *, size_t *);
uint8_t vt_field_plan(uint16_t,int,int,size_t,int,size_t,size_t);
uint32_t vt_color_index(uint32_t,uint32_t);
uint32_t vt_ycbcr_tables(int32_t,int32_t,int32_t,int32_t,int32_t);
int vt_commit(uint8_t *,size_t,size_t,const uint8_t *,size_t);
int64_t vt_strips(void *);
int vt_check_tile(void *,uint32_t,uint32_t,uint32_t,uint16_t);
int vt_compute_tile(void *,uint32_t,uint32_t,uint32_t,uint16_t,uint32_t *);
int vt_compute_strip(void *,uint32_t,uint16_t,uint32_t *);
/* Image positions, and the reads that set them (compat/src/index.rs). */
enum { VT_ROW, VT_STRIP, VT_TILE };
uint32_t vt_position(void *,uint32_t);
void vt_advance(void *,uint32_t,uint32_t,uint32_t);
#endif
