/* Differential check against the pinned libtiff. The same harness runs this
 * library and a renamed static libtiff over each input. Where both succeed on
 * the same call, the results and output bytes must agree. Failures on one side
 * only are expected: the verified reader rejects inputs libtiff tolerates.
 * VTIFF_FUZZ_CENSUS=1 prints every disagreement, including one-sided success,
 * instead of aborting on the first two-sided one. */
#include "transcript.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int vtiff_fuzz_one(const uint8_t *, size_t);
int vtiff_reference_one(const uint8_t *, size_t);
const struct transcript *vtiff_fuzz_transcript(void);
const struct transcript *vtiff_reference_transcript(void);

static struct transcript ours;
static const char *const kinds[] = {
    "?",    "open", "field",    "geometry", "encoded", "short", "whole", "raw",
    "user", "tile", "scanline", "rgba",     "next",    "count", "codec"};

static int by_key(const void *a, const void *b) {
  uint64_t x = ((const struct entry *)a)->key,
           y = ((const struct entry *)b)->key;
  return x < y ? -1 : x > y;
}
static unsigned kind_of(uint64_t key) {
  return (unsigned)(key >> 32) & 0xffffff;
}

/* Comparisons whose libtiff side is not a reference. Lossy JPEG/OJPEG pixels
 * have documented integer-transform differences, libtiff's JPEG/OJPEG sizes
 * can be for native subsampled data the shim does not produce, the shim's
 * LogLuv data format defaults to float XYZ (docs/c-api.md), and libtiff draws
 * separate MinIsWhite planes without inverting them (tif_getimage.c,
 * PickSeparateCase). */
static int64_t codec_entry(const struct transcript *t, uint64_t key,
                           unsigned index) {
  struct entry probe = {
      (key & 0xff00000000000000u) | (uint64_t)K_CODEC << 32 | index, 0, 0};
  const struct entry *codec =
      bsearch(&probe, t->entries, t->count, sizeof probe, by_key);
  return codec ? codec->value : 0;
}
static int documented(const struct transcript *t, uint64_t key) {
  int64_t compression = codec_entry(t, key, 0);
  unsigned kind = kind_of(key), index = (unsigned)(key & 0xffffffffu);
  if (kind == K_RGBA && codec_entry(t, key, 1))
    return 1;
  int pixels = kind >= K_ENCODED && kind <= K_RGBA && kind != K_RAW;
  if (compression == 34676 || compression == 34677)
    return pixels || kind == K_GEOMETRY ||
           (kind == K_FIELD && (index == 258 || index == 339));
  if ((compression == 6 || compression == 7) && kind == K_GEOMETRY &&
      index <= 7)
    return 1;
  return (compression == 6 || compression == 7) && pixels;
}

static void report(const char *what, const struct entry *a,
                   const struct entry *b, int census) {
  unsigned kind = kind_of(a->key);
  fprintf(stderr,
          "differential %s: scope %#x %s %#llx: ours %lld/%016llx, libtiff "
          "%lld/%016llx\n",
          what, (unsigned)(a->key >> 56),
          kind < sizeof kinds / sizeof kinds[0] ? kinds[kind] : "?",
          (unsigned long long)(a->key & 0xffffffffu), (long long)a->value,
          (unsigned long long)a->hash, (long long)b->value,
          (unsigned long long)b->hash);
  if (!census)
    abort();
}

int vtiff_diff_one(const uint8_t *data, size_t size) {
  static int census = -1;
  if (census < 0)
    census = getenv("VTIFF_FUZZ_CENSUS") != NULL;
  vtiff_fuzz_one(data, size);
  memcpy(&ours, vtiff_fuzz_transcript(), sizeof ours);
  vtiff_reference_one(data, size);
  static struct transcript reference;
  memcpy(&reference, vtiff_reference_transcript(), sizeof reference);
  qsort(ours.entries, ours.count, sizeof ours.entries[0], by_key);
  qsort(reference.entries, reference.count, sizeof reference.entries[0],
        by_key);
  for (size_t i = 0; i < ours.count; i++) {
    const struct entry *a = &ours.entries[i];
    const struct entry *b =
        bsearch(a, reference.entries, reference.count, sizeof *a, by_key);
    if (!b)
      continue;
    unsigned kind = kind_of(a->key), index = (unsigned)(a->key & 0xffffffffu);
    int both = 0, one = 0;
    switch (kind) {
    case K_FIELD:
      both = a->value == 1 && b->value == 1;
      one = (a->value == 1) != (b->value == 1);
      break;
    case K_GEOMETRY:
    case K_CODEC: {
      /* Sizes and TIFFCheckTile fail as 0, the strip and tile indexes as
       * UINT32_MAX. Flags, offsets and counts have no failure value. */
      int64_t failed =
          kind == K_GEOMETRY && (index == 8 || index == 9) ? UINT32_MAX : 0;
      if (kind == K_GEOMETRY && index > 10) {
        both = 1;
        break;
      }
      both = a->value != failed && b->value != failed;
      one = (a->value != failed) != (b->value != failed);
      break;
    }
    case K_OPEN:
    case K_NEXT:
    case K_COUNT:
      one = a->value != b->value;
      break;
    default:
      both = a->value > 0 && b->value > 0;
      one = (a->value > 0) != (b->value > 0);
    }
    if (documented(&ours, a->key))
      both = one = 0;
    if (both && (a->value != b->value || a->hash != b->hash))
      report("mismatch", a, b, census);
    else if (one && census)
      report("one-sided", a, b, census);
  }
  return 0;
}
