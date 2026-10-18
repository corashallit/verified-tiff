/* Read-side extension hooks. Caller callbacks and foreign pointers are outside
 * the verified core; client info is kept, and charged, by the owning handle. */
TIFFTagMethods *TIFFAccessTagMethods(TIFF *t) {
  return valid(t, __func__) ? &t->methods : NULL;
}
void *TIFFGetClientInfo(TIFF *t, const char *name) {
  if (!valid(t, __func__) || !name)
    return NULL;
  return vt_client_info(t->state, name);
}
void TIFFSetClientInfo(TIFF *t, void *data, const char *name) {
  if (!valid(t, __func__) || !name)
    return;
  if (!vt_set_client_info(t->state, data, name))
    bridge_fail(t, __func__);
}
int TIFFReadBufferSetup(TIFF *t, void *buffer, tmsize_t size) {
  if (!valid(t, __func__))
    return 0;
  if (!vt_read_buffer(t->state, buffer, size)) {
    bridge_fail(t, __func__);
    return 0;
  }
  return 1;
}
int TIFFUnsetField(TIFF *t, uint32_t tag) {
  if (!valid(t, __func__) || !TIFFFieldWithTag(t, tag))
    return 0;
  if (!vt_unset(t->state, tag)) {
    bridge_fail(t, __func__);
    return 0;
  }
  return 1;
}
void TIFFFreeDirectory(TIFF *t) {
  if (!valid(t, __func__))
    return;
  if (!vt_free_directory(t->state)) {
    bridge_fail(t, __func__);
    return;
  }
}
static int reader_codec_init(TIFF *t, int scheme) {
  if (!valid(t, "TIFF codec init"))
    return 0;
  if (!vt_codec_init(t->state, (uint16_t)scheme)) {
    bridge_fail(t, "TIFF codec init");
    return 0;
  }
  return TIFFIsCODECConfigured((uint16_t)scheme);
}
static const TIFFCodec reader_codecs[] = {
    {"None", 1, reader_codec_init},
    {"CCITT RLE", 2, reader_codec_init},
    {"CCITT Group 3", 3, reader_codec_init},
    {"CCITT Group 4", 4, reader_codec_init},
    {"LZW", 5, reader_codec_init},
    {"Old-style JPEG", 6, reader_codec_init},
    {"JPEG", 7, reader_codec_init},
    {"AdobeDeflate", 8, reader_codec_init},
    {"PackBits", 32773, reader_codec_init},
    {"Deflate", 32946, reader_codec_init},
    {"SGILog", 34676, reader_codec_init},
    {"LERC", 34887, reader_codec_init},
    {"ZSTD", 50000, reader_codec_init},
    {"WEBP", 50001, reader_codec_init},
    {NULL, 0, NULL}};
const TIFFCodec *TIFFFindCODEC(uint16_t scheme) {
  for (size_t i = 0; reader_codecs[i].name; i++)
    if (reader_codecs[i].scheme == scheme)
      return reader_codecs + i;
  return NULL;
}
TIFFCodec *TIFFGetConfiguredCODECs(void) {
  TIFFCodec *p = _TIFFmalloc(sizeof(reader_codecs));
  if (p)
    memcpy(p, reader_codecs, sizeof(reader_codecs));
  return p;
}
/* compat/src/print.rs formats the text. */
void TIFFPrintDirectory(TIFF *t, FILE *file, long flags) {
  if (!valid(t, __func__) || !file)
    return;
  size_t n = 0;
  const uint8_t *text = vt_print_directory(t->state, flags, &n);
  if (!text) {
    bridge_fail(t, __func__);
    return;
  }
  fwrite(text, 1, n, file);
  if (t->methods.printdir)
    t->methods.printdir(t, file, flags);
}
/* TIFFFieldArray is opaque in tiffio.h. Programs supplying it use the pinned
 * libtiff internal ABI, which compat/src/fields.rs decodes. */
int TIFFReadCustomDirectory(TIFF *t, toff_t off, const TIFFFieldArray *array) {
  if (!valid(t, __func__))
    return 0;
  if (!vt_read_custom_directory(t->state, off, array)) {
    bridge_fail(t, __func__);
    return 0;
  }
  return 1;
}

int TIFFSetFileno(TIFF *t, int fd) {
  if (!valid(t, __func__))
    return -1;
  int old = t->fd;
  t->fd = fd;
  return old;
}
int TIFFSetMode(TIFF *t, int mode) {
  if (!valid(t, __func__))
    return -1;
  int old = t->mode;
  t->mode = mode;
  return old;
}
thandle_t TIFFSetClientdata(TIFF *t, thandle_t client) {
  if (!valid(t, __func__))
    return NULL;
  if (!vt_clientdata(t->state, client)) {
    bridge_fail(t, __func__);
    return NULL;
  }
  thandle_t old = t->client;
  t->client = client;
  return old;
}
