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
/* Open options, modes and limits (compat/src/open.rs). */
struct vt_options {
  size_t input, output, work, single, cumulative;
  int warn_unknown, unsupported;
  uint32_t invalid_env;
};
_Static_assert(sizeof(struct vt_options) == 56, "options layout");
enum { VT_CHOP = 1, VT_HEADER = 2, VT_WRITE = 4, VT_BIGTIFF = 8, VT_BIGENDIAN = 16, VT_DEFERRED = 32 };
void vt_options_default(struct vt_options *, char *(*)(const char *));
int vt_options_limits(struct vt_options *, uint64_t, uint64_t, uint64_t);
int vt_options_allocation(struct vt_options *, int, int64_t);
int64_t vt_mode_flags(const char *);
size_t vt_admit(const struct vt_options *, const char *, size_t);
void *vt_open_fd(int, const struct vt_options *, size_t, uint32_t);
void *vt_open_client(thandle_t, TIFFReadWriteProc, TIFFReadWriteProc, TIFFSeekProc, uint64_t, const struct vt_options *, size_t, uint32_t);
int vt_next(void *);
int vt_clientdata(void *,thandle_t);
int vt_unset(void *,uint32_t);
int vt_free_directory(void *);
int vt_strile(void *,uint32_t,int,uint64_t *);
void vt_close(void *);
int vt_custom_dir(void *, uint64_t, uint32_t);
int vt_error_code(void);
const char *vt_error_message(void);
int vt_select(void *, uint32_t);
/* What vt_info reports of the current directory. */
enum { VT_INFO_DIRECTORY, VT_INFO_OFFSET, VT_INFO_NEXT, VT_INFO_TILED, VT_INFO_LITTLE_ENDIAN, VT_INFO_BIGTIFF, VT_INFO_SEGMENTS };
uint64_t vt_info(void *, uint32_t);
uint32_t vt_count_dirs(void *);
int vt_set_offset(void *, uint64_t);
enum { VT_SIZE_SCANLINE, VT_SIZE_STRIP, VT_SIZE_TILE_ROW, VT_SIZE_TILE, VT_SIZE_RASTER_SCANLINE };
int64_t vt_size(void *, uint32_t, uint32_t);
int vt_rgba_ok(void *);
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
uint32_t vt_color_index(uint32_t,uint32_t);
uint32_t vt_ycbcr_tables(int32_t,int32_t,int32_t,int32_t,int32_t);
int64_t vt_strips(void *);
int vt_check_tile(void *,uint32_t,uint32_t,uint32_t,uint16_t);
int vt_compute_tile(void *,uint32_t,uint32_t,uint32_t,uint16_t,uint32_t *);
int vt_compute_strip(void *,uint32_t,uint16_t,uint32_t *);
/* The values TIFFGetField writes through the caller's pointers, or
   TIFFSetField reads from its arguments, and their C types. */
enum { VT_U8, VT_I8, VT_U16, VT_I16, VT_U32, VT_I32, VT_U64, VT_I64, VT_FLOAT, VT_DOUBLE, VT_POINTER };
struct vt_value {
  uint32_t type;
  uint64_t bits;
  const void *pointer;
};
struct vt_plan {
  size_t count;
  struct vt_value value[3];
};
_Static_assert(sizeof(struct vt_value) == 24 && sizeof(struct vt_plan) == 80, "plan layout");
int vt_get(void *, uint32_t, int, struct vt_plan *);
/* TIFFSetField's argument types, and the field set from the arguments
   (compat/src/set.rs). */
int vt_set_plan(void *, uint32_t, struct vt_plan *);
int vt_set(void *, uint32_t, const struct vt_plan *);
/* Image positions, which reads set (compat/src/index.rs). */
enum { VT_ROW, VT_STRIP, VT_TILE };
uint32_t vt_position(void *,uint32_t);
/* Reads into the caller's buffers (compat/src/read.rs). */
enum { VT_READ_SCANLINE, VT_READ_STRIP, VT_READ_TILE, VT_READ_RAW_STRIP, VT_READ_RAW_TILE };
int64_t vt_read_into(void *, uint32_t, uint32_t, uint32_t, void *, int64_t);
enum { VT_RGBA_IMAGE, VT_RGBA_STRIP, VT_RGBA_TILE };
int vt_rgba_into(void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *);
int vt_region_into(void *, int32_t, int32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *);
int vt_user_buffer_into(void *, uint32_t, const void *, int64_t, void *, int64_t);
int vt_read_buffer(void *, void *, int64_t);
int64_t vt_raw_size(void *, uint32_t);
/* The fields of TIFFRGBAImage that TIFFRGBAImageBegin fills. */
struct vt_rgba {
  uint32_t width, height;
  uint16_t orientation, bits, samples, photometric, alpha, contig;
  const uint16_t *colormap[3];
};
_Static_assert(sizeof(struct vt_rgba) == 48, "RGBA state layout");
int vt_rgba_begin(void *, struct vt_rgba *);
int vt_upsampled(void *);
/* Client info, byte utilities and data widths (compat/src/utility.rs). */
void *vt_client_info(void *, const char *);
int vt_set_client_info(void *, void *, const char *);
int vt_swab(void *, int64_t, size_t);
int vt_reverse_bits(uint8_t *, int64_t);
const unsigned char *vt_bit_table(int);
int vt_data_width(int);
/* Codec init (compat/src/get.rs) and the directory's text (print.rs). */
int vt_codec_init(void *, uint16_t);
const uint8_t *vt_print_directory(void *, int64_t, size_t *);
/* The directory writer (compat/src/writer.rs). */
int vt_write_check(void *, int);
int vt_write_directory(void *);
int vt_flush(void *);
#endif
