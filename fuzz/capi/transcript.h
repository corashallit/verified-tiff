/* Calls recorded by harness.c for the differential comparison in diff.c. */
#ifndef VTIFF_FUZZ_TRANSCRIPT_H
#define VTIFF_FUZZ_TRANSCRIPT_H
#include <stddef.h>
#include <stdint.h>
enum {
  K_OPEN = 1,
  K_FIELD,
  K_GEOMETRY,
  K_ENCODED,
  K_SHORT,
  K_WHOLE,
  K_RAW,
  K_USER,
  K_TILE,
  K_SCANLINE,
  K_RGBA,
  K_NEXT,
  K_COUNT,
  K_CODEC
};
#define TRANSCRIPT_ENTRIES 8192
/* key: directory scope (top byte), call kind (bits 32..55), call index. */
struct entry {
  uint64_t key;
  int64_t value;
  uint64_t hash;
};
struct transcript {
  size_t count;
  struct entry entries[TRANSCRIPT_ENTRIES];
};
#endif
