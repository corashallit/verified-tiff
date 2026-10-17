/* Coverage-guided fuzzing of the public C API over an in-memory file.
 * Each input is opened through TIFFClientOpenExt with small limits, then its
 * directories, fields, borrowed arrays, geometry and every read path are
 * exercised. AddressSanitizer checks the C shim, the Rust adapter and borrowed
 * caller buffers; result checks catch reads that claim more than they wrote.
 *
 * Every call also appends (key, result, output hash) to a transcript. Built
 * with -DVTIFF_REFERENCE, the same code drives a renamed copy of the pinned
 * libtiff instead, so diff.c can compare the two transcripts. */
#ifdef VTIFF_REFERENCE
#include "reference_names.h"
#define ENTRY vtiff_reference_one
#define TRANSCRIPT vtiff_reference_transcript
#else
#define ENTRY vtiff_fuzz_one
#define TRANSCRIPT vtiff_fuzz_transcript
#endif
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tiffio.h>
#ifndef VTIFF_REFERENCE
#include <verified_tiff.h>
#endif
#include "transcript.h"

#define MAX_BUFFER ((tmsize_t)4 << 20)
#define MAX_PIXELS (1u << 18)
#define MAX_SEGMENTS 6u
#define MAX_ROWS 48u
#define MAX_DIRECTORIES 4

static const uint32_t tags[] = {
#include "tags.inc"
};

struct memory {
  const uint8_t *data;
  size_t size, at;
};
static volatile uint64_t sink;
static FILE *null_output;
static int trace;
static struct transcript log_;
static uint64_t scope;
/* Set once a forward scanline pass on this handle has failed. */
static int scan_failed;

static void fail(const char *what) {
  fprintf(stderr, "fuzz contract failure: %s\n", what);
  abort();
}
/* Read every byte (ASan checks the extent) and return an FNV-1a hash. */
static uint64_t touch(const void *p, size_t n) {
  const volatile uint8_t *b = p;
  uint64_t h = 0xcbf29ce484222325u;
  for (size_t i = 0; i < n; i++)
    h = (h ^ b[i]) * 0x100000001b3u;
  sink += h;
  return h;
}
static void record(unsigned kind, uint64_t index, int64_t value,
                   uint64_t hash) {
  if (log_.count < TRANSCRIPT_ENTRIES)
    log_.entries[log_.count++] =
        (struct entry){scope | (uint64_t)kind << 32 | index, value, hash};
}
/* Formatting every diagnostic catches format/argument mismatches. */
static void format(const char *module, const char *fmt, va_list ap) {
  char text[1024];
  vsnprintf(text, sizeof text, fmt, ap);
  if (trace)
    fprintf(stderr, "diagnostic %s: %s\n", module ? module : "-", text);
  touch(text, strlen(text));
  if (module)
    touch(module, strlen(module));
}
static int handler(TIFF *t, void *data, const char *module, const char *fmt,
                   va_list ap) {
  (void)t;
  (void)data;
  format(module, fmt, ap);
  return 1;
}

static tmsize_t read_mem(thandle_t h, void *out, tmsize_t n) {
  struct memory *m = h;
  if (n < 0)
    return -1;
  size_t take = (size_t)n;
  if (take > m->size - m->at)
    take = m->size - m->at;
  memcpy(out, m->data + m->at, take);
  m->at += take;
  return (tmsize_t)take;
}
static tmsize_t write_mem(thandle_t h, void *in, tmsize_t n) {
  (void)h;
  (void)in;
  (void)n;
  return -1;
}
static toff_t seek_mem(thandle_t h, toff_t off, int whence) {
  struct memory *m = h;
  uint64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? m->at : m->size;
  if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END)
    return (toff_t)-1;
  if (off > m->size - base)
    return (toff_t)-1;
  m->at = (size_t)(base + off);
  return m->at;
}
static int close_mem(thandle_t h) {
  (void)h;
  return 0;
}
static toff_t size_mem(thandle_t h) { return ((struct memory *)h)->size; }
static int map_mem(thandle_t h, void **base, toff_t *size) {
  (void)h;
  (void)base;
  (void)size;
  return 0;
}
static void unmap_mem(thandle_t h, void *base, toff_t size) {
  (void)h;
  (void)base;
  (void)size;
}

/* Fetch a field with libtiff's documented argument convention and oversized
 * output slots, then read every element the declared count promises. */
static void field(TIFF *t, uint32_t tag, uint16_t bits, uint16_t spp) {
  const TIFFField *f = TIFFFieldWithTag(t, tag);
  if (!f)
    return;
  if (trace)
    fprintf(stderr, "field %u %s\n", tag, TIFFFieldName(f));
  uint64_t a[4] = {0}, b[4] = {0}, c[4] = {0};
  int ok = 0;
  uint64_t h = 0;
  switch (tag) {
  case TIFFTAG_STRIPOFFSETS:
  case TIFFTAG_STRIPBYTECOUNTS:
  case TIFFTAG_TILEOFFSETS:
  case TIFFTAG_TILEBYTECOUNTS:
    if ((ok = TIFFGetField(t, tag, a)) == 1 && a[0]) {
      uint32_t n =
          TIFFIsTiled(t) ? TIFFNumberOfTiles(t) : TIFFNumberOfStrips(t);
      h = touch((const void *)(uintptr_t)a[0], (size_t)n * 8);
    }
    break;
  case TIFFTAG_COLORMAP:
    if ((ok = TIFFGetField(t, tag, a, b, c)) == 1 && bits <= 16) {
      size_t n = (size_t)1 << bits;
      h = touch((const void *)(uintptr_t)a[0], n * 2) ^
          touch((const void *)(uintptr_t)b[0], n * 2) * 3 ^
          touch((const void *)(uintptr_t)c[0], n * 2) * 5;
    }
    break;
  case TIFFTAG_TRANSFERFUNCTION: {
    uint16_t extra = 0, *values = NULL;
    TIFFGetFieldDefaulted(t, TIFFTAG_EXTRASAMPLES, &extra, &values);
    if ((ok = TIFFGetField(t, tag, a, b, c)) == 1 && bits <= 16) {
      size_t n = (size_t)1 << bits;
      h = touch((const void *)(uintptr_t)a[0], n * 2);
      if (spp - extra > 1)
        h ^= touch((const void *)(uintptr_t)b[0], n * 2) * 3 ^
             touch((const void *)(uintptr_t)c[0], n * 2) * 5;
    }
    break;
  }
  case TIFFTAG_EXTRASAMPLES:
  case TIFFTAG_SUBIFD: {
    uint16_t n = 0;
    if ((ok = TIFFGetField(t, tag, &n, a)) == 1 && n)
      h = touch((const void *)(uintptr_t)a[0],
                (size_t)n * (tag == TIFFTAG_SUBIFD ? 8 : 2));
    h ^= n;
    break;
  }
  case TIFFTAG_PAGENUMBER:
  case TIFFTAG_YCBCRSUBSAMPLING:
  case TIFFTAG_HALFTONEHINTS:
  case TIFFTAG_DOTRANGE:
    ok = TIFFGetField(t, tag, a, b);
    h = touch(a, sizeof a) ^ touch(b, sizeof b) * 3;
    break;
  /* libtiff's standard and codec scalars, whatever their read counts. */
  case TIFFTAG_SUBFILETYPE:
  case TIFFTAG_IMAGEWIDTH:
  case TIFFTAG_IMAGELENGTH:
  case TIFFTAG_BITSPERSAMPLE:
  case TIFFTAG_COMPRESSION:
  case TIFFTAG_PHOTOMETRIC:
  case TIFFTAG_THRESHHOLDING:
  case TIFFTAG_FILLORDER:
  case TIFFTAG_ORIENTATION:
  case TIFFTAG_SAMPLESPERPIXEL:
  case TIFFTAG_ROWSPERSTRIP:
  case TIFFTAG_MINSAMPLEVALUE:
  case TIFFTAG_MAXSAMPLEVALUE:
  case TIFFTAG_SMINSAMPLEVALUE:
  case TIFFTAG_SMAXSAMPLEVALUE:
  case TIFFTAG_XRESOLUTION:
  case TIFFTAG_YRESOLUTION:
  case TIFFTAG_PLANARCONFIG:
  case TIFFTAG_XPOSITION:
  case TIFFTAG_YPOSITION:
  case TIFFTAG_RESOLUTIONUNIT:
  case TIFFTAG_MATTEING:
  case TIFFTAG_TILEWIDTH:
  case TIFFTAG_TILELENGTH:
  case TIFFTAG_TILEDEPTH:
  case TIFFTAG_DATATYPE:
  case TIFFTAG_SAMPLEFORMAT:
  case TIFFTAG_IMAGEDEPTH:
  case TIFFTAG_YCBCRPOSITIONING:
  case TIFFTAG_NUMBEROFINKS:
  case TIFFTAG_PREDICTOR:
  case TIFFTAG_GROUP3OPTIONS:
  case TIFFTAG_GROUP4OPTIONS:
  case TIFFTAG_BADFAXLINES:
  case TIFFTAG_CLEANFAXDATA:
  case TIFFTAG_CONSECUTIVEBADFAXLINES:
  case TIFFTAG_FAXRECVPARAMS:
  case TIFFTAG_FAXRECVTIME:
    ok = TIFFGetField(t, tag, a);
    h = touch(a, sizeof a);
    break;
  case TIFFTAG_JPEGTABLES: {
    uint32_t n = 0;
    if ((ok = TIFFGetField(t, tag, &n, a)) == 1 && n)
      h = touch((const void *)(uintptr_t)a[0], n);
    break;
  }
  case TIFFTAG_REFERENCEBLACKWHITE:
    if ((ok = TIFFGetField(t, tag, a)) == 1 && a[0])
      h = touch((const void *)(uintptr_t)a[0], 6 * sizeof(float));
    break;
  case TIFFTAG_INKNAMES:
    if ((ok = TIFFGetField(t, tag, a)) == 1 && a[0])
      h = touch((const void *)(uintptr_t)a[0],
                strlen((const char *)(uintptr_t)a[0]));
    break;
  default: {
    int size = TIFFFieldSetGetSize(f);
    int count = TIFFFieldReadCount(f);
    TIFFDataType type = TIFFFieldDataType(f);
    if (size <= 0 || size > 8)
      size = 8;
    if (TIFFFieldPassCount(f)) {
      uint32_t n = 0;
      void *data = NULL;
      if (count == TIFF_VARIABLE2) {
        ok = TIFFGetField(t, tag, &n, &data);
      } else {
        uint16_t small = 0;
        ok = TIFFGetField(t, tag, &small, &data);
        n = small;
      }
      if (ok != 1)
        break;
      if (n && !data)
        fail("counted field without data");
      if (data)
        h = touch(data, (size_t)n * (size_t)size);
      h ^= n;
    } else if (type == TIFF_ASCII || count == TIFF_VARIABLE ||
               count == TIFF_VARIABLE2 || count == TIFF_SPP || count > 1) {
      void *data = NULL;
      if ((ok = TIFFGetField(t, tag, &data)) != 1 || !data)
        break;
      if (type == TIFF_ASCII)
        h = touch(data, strlen(data) + 1);
      else if (count == TIFF_SPP)
        h = touch(data, (size_t)spp * (size_t)size);
      else if (count > 1)
        h = touch(data, (size_t)count * (size_t)size);
    } else {
      ok = TIFFGetField(t, tag, a);
      h = touch(a, sizeof a);
    }
  }
  }
  record(K_FIELD, tag, ok, ok == 1 ? h : 0);
}

static void *alloc(tmsize_t n) {
  if (n <= 0 || n > MAX_BUFFER)
    return NULL;
  return calloc(1, (size_t)n);
}

/* Bits after a row's last sample are padding. libtiff's fax decoder leaves
 * them as the buffer held them, so they are cleared before hashing. */
static uint64_t row_bits;
static size_t row_bytes;
static void padding(TIFF *t) {
  int tiled = TIFFIsTiled(t);
  uint32_t width = 0;
  uint16_t bits = 0, spp = 1, planar = PLANARCONFIG_CONTIG;
  TIFFGetField(t, tiled ? TIFFTAG_TILEWIDTH : TIFFTAG_IMAGEWIDTH, &width);
  TIFFGetFieldDefaulted(t, TIFFTAG_BITSPERSAMPLE, &bits);
  TIFFGetFieldDefaulted(t, TIFFTAG_SAMPLESPERPIXEL, &spp);
  TIFFGetFieldDefaulted(t, TIFFTAG_PLANARCONFIG, &planar);
  row_bits =
      (uint64_t)width * bits * (planar == PLANARCONFIG_SEPARATE ? 1 : spp);
  row_bytes = (size_t)(tiled ? TIFFTileRowSize64(t) : TIFFScanlineSize64(t));
  if (row_bits % 8 == 0 || row_bytes != (row_bits + 7) / 8)
    row_bytes = 0;
}
static void result(unsigned kind, uint64_t index, tmsize_t got, void *buf) {
  if (got > 0 && row_bytes && kind != K_RAW && kind != K_RGBA)
    for (size_t at = row_bytes; at <= (size_t)got; at += row_bytes)
      ((uint8_t *)buf)[at - 1] &= (uint8_t)(0xff00u >> (row_bits % 8));
  record(kind, index, got, got > 0 ? touch(buf, (size_t)got) : 0);
}

static void segments(TIFF *t) {
  int tiled = TIFFIsTiled(t);
  uint32_t n = tiled ? TIFFNumberOfTiles(t) : TIFFNumberOfStrips(t);
  tmsize_t size = tiled ? TIFFTileSize(t) : TIFFStripSize(t);
  tmsize_t row = tiled ? TIFFTileRowSize(t) : TIFFScanlineSize(t);
  uint8_t *buf = alloc(size);
  if (!buf)
    return;
  uint64_t *counts = NULL;
  TIFFGetField(t, tiled ? TIFFTAG_TILEBYTECOUNTS : TIFFTAG_STRIPBYTECOUNTS,
               &counts);
  for (uint32_t i = 0; i < n && i < MAX_SEGMENTS; i++) {
    tmsize_t got = tiled ? TIFFReadEncodedTile(t, i, buf, size)
                         : TIFFReadEncodedStrip(t, i, buf, size);
    if (got > size)
      fail("encoded read longer than its buffer");
    result(K_ENCODED, i, got, buf);
    /* A short caller buffer must bound the copy. Several libtiff codecs
     * assert that it holds whole rows. */
    tmsize_t half = row > 0 ? size / 2 / row * row : 0;
    uint8_t *small = alloc(half);
    if (small) {
      got = tiled ? TIFFReadEncodedTile(t, i, small, half)
                  : TIFFReadEncodedStrip(t, i, small, half);
      if (got > half)
        fail("short encoded read longer than its buffer");
      result(K_SHORT, i, got, small);
      free(small);
    }
    if (!tiled) {
      got = TIFFReadEncodedStrip(t, i, buf, (tmsize_t)-1);
      if (got > size)
        fail("whole-strip read longer than TIFFStripSize");
      result(K_WHOLE, i, got, buf);
    }
    if (counts && counts[i] && counts[i] <= (uint64_t)MAX_BUFFER) {
      tmsize_t raw_size = (tmsize_t)counts[i];
      uint8_t *raw = alloc(raw_size);
      if (raw) {
        got = tiled ? TIFFReadRawTile(t, i, raw, raw_size)
                    : TIFFReadRawStrip(t, i, raw, raw_size);
        if (got > raw_size)
          fail("raw read longer than its buffer");
        result(K_RAW, i, got, raw);
        if (got > 0) {
          int ok = TIFFReadFromUserBuffer(t, i, raw, got, buf, size);
          result(K_USER, i, ok ? size : 0, buf);
        }
        free(raw);
      }
    }
  }
  if (tiled) {
    uint32_t tw = 0, th = 0;
    TIFFGetField(t, TIFFTAG_TILEWIDTH, &tw);
    TIFFGetField(t, TIFFTAG_TILELENGTH, &th);
    tmsize_t got = TIFFReadTile(t, buf, 0, 0, 0, 0);
    if (got > size)
      fail("tile read longer than TIFFTileSize");
    result(K_TILE, 0, got, buf);
    if (tw && th) {
      got = TIFFReadTile(t, buf, tw, th, 0, 0);
      result(K_TILE, 1, got, buf);
    }
  }
  free(buf);
}

static void scanlines(TIFF *t, uint32_t height) {
  if (TIFFIsTiled(t))
    return;
  tmsize_t size = TIFFScanlineSize(t);
  uint8_t *buf = alloc(size);
  if (!buf)
    return;
  uint16_t planar = 1, spp = 1;
  TIFFGetFieldDefaulted(t, TIFFTAG_PLANARCONFIG, &planar);
  TIFFGetFieldDefaulted(t, TIFFTAG_SAMPLESPERPIXEL, &spp);
  uint16_t planes = planar == PLANARCONFIG_SEPARATE ? spp : 1;
  int failed = 0, read = 0, late = 0;
  for (uint16_t s = 0; s < planes && s < 4; s++)
    for (uint32_t y = 0; y < height && y < MAX_ROWS; y++) {
      int ok = TIFFReadScanline(t, buf, y, s);
      result(K_SCANLINE, (uint64_t)s << 16 | y, ok == 1 ? size : ok, buf);
      if (ok != 1) {
        late |= read;
        failed = 1;
        break;
      }
      read = 1;
    }
  /* libtiff can return stale bytes for the read below once forward passes
   * have failed in two directories of the handle, or once one has failed
   * after reading a row: the failed strip fill overwrites the raw data of
   * the strip it leaves current (tif_read.c, TIFFFillStripPartial). */
  record(K_CODEC, 2, failed && (scan_failed || late), 0);
  scan_failed |= failed;
  /* Backward access after a forward pass. */
  if (height > 1) {
    int ok = TIFFReadScanline(t, buf, 0, 0);
    result(K_SCANLINE, 0xffff, ok == 1 ? size : ok, buf);
  }
  free(buf);
}

static void rgba(TIFF *t, uint32_t w, uint32_t h) {
  char message[1024];
  if (!w || !h || w > MAX_PIXELS / h)
    return;
  int usable = TIFFRGBAImageOK(t, message);
  record(K_RGBA, 0, usable, 0);
  if (!usable)
    return;
  uint32_t *full = calloc((size_t)w * h, 4), *raster = calloc((size_t)w * h, 4);
  if (!full || !raster) {
    free(full);
    free(raster);
    return;
  }
  int whole = TIFFReadRGBAImageOriented(t, w, h, full, ORIENTATION_TOPLEFT, 0);
  result(K_RGBA, 1, whole ? (tmsize_t)w * h * 4 : 0, full);
  /* libtiff 4.7.1 misplaces cropped RGBA reads of several layouts: it
   * offsets columns by one byte per sample (tif_getimage.c:1236), truncates
   * sub-byte row skews (:1665), does not scale gray+alpha skews, and reads
   * past its buffer for a column offset into 2x2 YCbCr tiles (:837). Only
   * this library runs crops, and each must match the same region of its own
   * full read. */
#ifndef VTIFF_REFERENCE
  uint16_t orientation = ORIENTATION_TOPLEFT;
  TIFFGetFieldDefaulted(t, TIFFTAG_ORIENTATION, &orientation);
  int check = whole && orientation == ORIENTATION_TOPLEFT;
  if (w > 2 && h > 2) {
    uint32_t rw = w / 2, rh = h / 2;
    int ok =
        TIFFReadRGBAImageOriented(t, rw, rh, raster, ORIENTATION_BOTLEFT, 0);
    result(K_RGBA, 2, ok ? (tmsize_t)rw * rh * 4 : 0, raster);
    for (uint32_t y = 0; ok && check && y < rh; y++)
      if (memcmp(raster + (size_t)(rh - 1 - y) * rw, full + (size_t)y * w,
                 (size_t)rw * 4))
        fail("cropped RGBA read differs from the full read");
  }
  TIFFRGBAImage img;
  if (TIFFRGBAImageBegin(&img, t, 0, message)) {
    img.req_orientation = ORIENTATION_TOPLEFT;
    img.row_offset = (int)(h / 3);
    img.col_offset = (int)(w / 3);
    uint32_t rw = w - w / 3, rh = h - h / 3;
    memset(raster, 0, (size_t)w * h * 4);
    int ok = TIFFRGBAImageGet(&img, raster, rw, rh);
    result(K_RGBA, 3, ok ? (tmsize_t)rw * rh * 4 : 0, raster);
    for (uint32_t y = 0; ok && check && y < rh; y++)
      if (memcmp(raster + (size_t)y * rw,
                 full + (size_t)(y + h / 3) * w + w / 3, (size_t)rw * 4))
        fail("offset RGBA read differs from the full read");
    TIFFRGBAImageEnd(&img);
  }
#endif
  free(full);
  free(raster);
  if (TIFFIsTiled(t)) {
    uint32_t tw = 0, th = 0;
    TIFFGetField(t, TIFFTAG_TILEWIDTH, &tw);
    TIFFGetField(t, TIFFTAG_TILELENGTH, &th);
    if (tw && th && tw <= MAX_PIXELS / th) {
      uint32_t *tile = calloc((size_t)tw * th, 4);
      if (tile) {
        int ok = TIFFReadRGBATile(t, 0, 0, tile);
        result(K_RGBA, 4, ok ? (tmsize_t)tw * th * 4 : 0, tile);
        free(tile);
      }
    }
  } else {
    uint32_t rows = h;
    TIFFGetFieldDefaulted(t, TIFFTAG_ROWSPERSTRIP, &rows);
    if (rows > h)
      rows = h;
    if (rows && w <= MAX_PIXELS / rows) {
      uint32_t *strip = calloc((size_t)w * rows, 4);
      if (strip) {
        int ok = TIFFReadRGBAStrip(t, 0, strip);
        result(K_RGBA, 4, ok ? (tmsize_t)w * rows * 4 : 0, strip);
        free(strip);
      }
    }
  }
}

static void fields(TIFF *t) {
  uint16_t bits = 0, spp = 1;
  TIFFGetFieldDefaulted(t, TIFFTAG_BITSPERSAMPLE, &bits);
  TIFFGetFieldDefaulted(t, TIFFTAG_SAMPLESPERPIXEL, &spp);
  for (size_t i = 0; i < sizeof tags / sizeof tags[0]; i++)
    field(t, tags[i], bits, spp);
  int custom = TIFFGetTagListCount(t);
  for (int i = 0; i < custom && i < 256; i++)
    field(t, TIFFGetTagListEntry(t, i), bits, spp);
}

static void directory(TIFF *t) {
  fields(t);
  if (null_output)
    TIFFPrintDirectory(t, null_output,
                       TIFFPRINT_STRIPS | TIFFPRINT_CURVES |
                           TIFFPRINT_COLORMAP);
  uint32_t w = 0, h = 0;
  uint16_t compression = 0;
  TIFFGetField(t, TIFFTAG_IMAGEWIDTH, &w);
  TIFFGetField(t, TIFFTAG_IMAGELENGTH, &h);
  TIFFGetFieldDefaulted(t, TIFFTAG_COMPRESSION, &compression);
  record(K_CODEC, 0, compression, 0);
  uint16_t photometric = 0, planar = PLANARCONFIG_CONTIG, spp = 1;
  int known = TIFFGetField(t, TIFFTAG_PHOTOMETRIC, &photometric);
  TIFFGetFieldDefaulted(t, TIFFTAG_PLANARCONFIG, &planar);
  TIFFGetFieldDefaulted(t, TIFFTAG_SAMPLESPERPIXEL, &spp);
  record(K_CODEC, 1,
         known && photometric == PHOTOMETRIC_MINISWHITE &&
             planar == PLANARCONFIG_SEPARATE && spp > 1,
         0);
  /* Consumers, like libtiff's own tools, ask JPEG to upsample YCbCr. */
  if (compression == COMPRESSION_JPEG && known &&
      photometric == PHOTOMETRIC_YCBCR)
    TIFFSetField(t, TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB);
  const uint64_t geometry[] = {
      TIFFScanlineSize64(t),
      TIFFRasterScanlineSize64(t),
      TIFFStripSize64(t),
      TIFFTileSize64(t),
      TIFFTileRowSize64(t),
      TIFFVStripSize64(t, 1),
      TIFFVTileSize64(t, 1),
      TIFFRawStripSize64(t, 0),
      TIFFComputeStrip(t, h ? h - 1 : 0, 0),
      TIFFComputeTile(t, w ? w - 1 : 0, h ? h - 1 : 0, 0, 0),
      (uint64_t)TIFFCheckTile(t, 0, 0, 0, 0),
      TIFFCurrentDirOffset(t),
      (uint64_t)TIFFIsByteSwapped(t),
      (uint64_t)TIFFIsUpSampled(t),
      (uint64_t)TIFFIsMSB2LSB(t),
      TIFFCurrentDirectory(t),
      (uint64_t)TIFFIsTiled(t),
      TIFFNumberOfStrips(t),
      TIFFNumberOfTiles(t)};
  for (size_t i = 0; i < sizeof geometry / sizeof geometry[0]; i++)
    record(K_GEOMETRY, i, (int64_t)geometry[i], 0);
  padding(t);
  /* libtiff continues, not restarts, a scanline read of row 0 after a
   * whole-strip decode of the same strip; scan first so both restart. */
  scanlines(t, h);
  segments(t);
  rgba(t, w, h);
  /* libtiff's Group 3 decoder keeps a missing-EOL switch for the strips
   * after the one that set it, and its strip and tile counts overflow to 0
   * for images of 2^31 rows or more. */
  int mode = 0;
  record(K_CODEC, 3,
         compression == COMPRESSION_CCITTFAX3 &&
             TIFFGetField(t, TIFFTAG_FAXMODE, &mode) && (mode & FAXMODE_NOEOL),
         0);
  record(K_CODEC, 4, !TIFFNumberOfStrips(t) || !TIFFNumberOfTiles(t), 0);
}

static void open_one(const uint8_t *data, size_t size, const char *mode) {
  struct memory m = {data, size, 0};
  TIFFOpenOptions *o = TIFFOpenOptionsAlloc();
  if (!o)
    return;
#ifndef VTIFF_REFERENCE
  VTIFFOpenOptionsSetLimits(o, size, 8 << 20, 50000000);
#endif
  TIFFOpenOptionsSetMaxSingleMemAlloc(o, 16 << 20);
  TIFFOpenOptionsSetMaxCumulatedMemAlloc(o, 64 << 20);
  TIFFOpenOptionsSetErrorHandlerExtR(o, handler, NULL);
  TIFFOpenOptionsSetWarningHandlerExtR(o, handler, NULL);
  TIFF *t =
      TIFFClientOpenExt("fuzz.tif", mode, &m, read_mem, write_mem, seek_mem,
                        close_mem, size_mem, map_mem, unmap_mem, o);
  TIFFOpenOptionsFree(o);
  record(K_OPEN, 0, t != NULL, 0);
  if (!t)
    return;
  for (int d = 0; d < MAX_DIRECTORIES; d++) {
    scope = (uint64_t)d << 56;
    directory(t);
    uint16_t n = 0;
    uint64_t *subifds = NULL;
    uint64_t exif = 0, gps = 0;
    TIFFGetField(t, TIFFTAG_SUBIFD, &n, &subifds);
    uint64_t first = n && subifds ? subifds[0] : 0;
    TIFFGetField(t, TIFFTAG_EXIFIFD, &exif);
    TIFFGetField(t, TIFFTAG_GPSIFD, &gps);
    tdir_t here = TIFFCurrentDirectory(t);
    if (first && TIFFSetSubDirectory(t, first)) {
      scope = (uint64_t)(0x10 + d) << 56;
      directory(t);
      TIFFSetDirectory(t, here);
    }
    if (exif && TIFFReadEXIFDirectory(t, exif)) {
      scope = (uint64_t)(0x20 + d) << 56;
      fields(t);
      TIFFSetDirectory(t, here);
    }
    if (gps && TIFFReadGPSDirectory(t, gps)) {
      scope = (uint64_t)(0x30 + d) << 56;
      fields(t);
      TIFFSetDirectory(t, here);
    }
    scope = (uint64_t)d << 56;
    int next = TIFFReadDirectory(t);
    record(K_NEXT, 0, next, 0);
    if (!next)
      break;
  }
  scope = (uint64_t)0x40 << 56;
  record(K_COUNT, 0, TIFFNumberOfDirectories(t), 0);
  int first = TIFFSetDirectory(t, 0);
  record(K_NEXT, 1, first, 0);
  if (first)
    directory(t);
  TIFFClose(t);
}

const struct transcript *TRANSCRIPT(void) { return &log_; }

int ENTRY(const uint8_t *data, size_t size) {
  static int ready;
  if (!ready) {
    TIFFSetErrorHandler(NULL);
    TIFFSetWarningHandler(NULL);
    null_output = fopen("/dev/null", "w");
    trace = getenv("VTIFF_FUZZ_TRACE") != NULL;
    ready = 1;
  }
  static const char *const modes[] = {"r", "rc", "rh", "rm", "rC"};
  const char *mode = modes[size ? data[size - 1] % 5 : 0];
  log_.count = 0;
  scan_failed = 0;
  scope = 0;
  open_one(data, size, mode);
  return 0;
}
