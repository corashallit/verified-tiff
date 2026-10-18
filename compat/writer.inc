/* A deliberately limited writer: serialize image metadata for applications
 * that construct temporary decoder contexts. Image encoding remains ENOSYS. */
int TIFFWriteCheck(TIFF *t, int tiles, const char *module) {
  (void)module;
  if (!valid(t, __func__))
    return 0;
  if (!vt_write_check(t->state, tiles)) {
    bridge_fail(t, __func__);
    return 0;
  }
  return 1;
}
int TIFFWriteDirectory(TIFF *t) {
  if (!valid(t, __func__))
    return 0;
  if (!vt_write_directory(t->state)) {
    bridge_fail(t, __func__);
    return 0;
  }
  return 1;
}
int TIFFFlushData(TIFF *t) { return valid(t, __func__); }
int TIFFFlush(TIFF *t) {
  if (!valid(t, __func__))
    return 0;
  if (!vt_flush(t->state)) {
    bridge_fail(t, __func__);
    return 0;
  }
  return 1;
}

int TIFFReadFromUserBuffer(TIFF *t, uint32_t k, void *input, tmsize_t n,
                           void *output, tmsize_t wanted) {
  if (!valid(t, __func__))
    return 0;
  if (!vt_user_buffer_into(t->state, k, input, n, output, wanted)) {
    bridge_fail(t, __func__);
    return 0;
  }
  return 1;
}
