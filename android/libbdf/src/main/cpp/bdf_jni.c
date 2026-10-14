/*
 * The JNI functions of libbdf for Android: what the class Native of the
 * Kotlin package calls. Handles are pointers in a long; strings cross in
 * both directions as the bytes of their UTF-8 (JNI's own string functions
 * use a modified UTF-8 that is not that of characters outside the BMP).
 * A failure throws BdfException with libbdf's status and message.
 */
#include <android/bitmap.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bdf/bdf.h"

#define FN(name) Java_io_github_shibukawa_bdf_Native_##name

static jclass g_exception;          /* io.github.shibukawa.bdf.BdfException */
static jmethodID g_exception_init;  /* (int, String) */
static jclass g_bitmap;             /* android.graphics.Bitmap */
static jmethodID g_bitmap_create;   /* createBitmap(int, int, Config) */
static jobject g_argb8888;          /* Bitmap.Config.ARGB_8888 */
static jclass g_bytes;              /* byte[] */
static jclass g_object;             /* Object */

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved) {
  (void)reserved;
  JNIEnv *env;
  if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
    return JNI_ERR;
  }
  jclass c = (*env)->FindClass(env, "io/github/shibukawa/bdf/BdfException");
  if (!c) {
    return JNI_ERR;
  }
  g_exception = (*env)->NewGlobalRef(env, c);
  g_exception_init = (*env)->GetMethodID(env, c, "<init>", "(ILjava/lang/String;)V");
  c = (*env)->FindClass(env, "android/graphics/Bitmap");
  if (!c) {
    return JNI_ERR;
  }
  g_bitmap = (*env)->NewGlobalRef(env, c);
  g_bitmap_create = (*env)->GetStaticMethodID(
      env, c, "createBitmap", "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;");
  c = (*env)->FindClass(env, "android/graphics/Bitmap$Config");
  if (!c) {
    return JNI_ERR;
  }
  jfieldID f = (*env)->GetStaticFieldID(env, c, "ARGB_8888", "Landroid/graphics/Bitmap$Config;");
  g_argb8888 = (*env)->NewGlobalRef(env, (*env)->GetStaticObjectField(env, c, f));
  g_bytes = (*env)->NewGlobalRef(env, (*env)->FindClass(env, "[B"));
  g_object = (*env)->NewGlobalRef(env, (*env)->FindClass(env, "java/lang/Object"));
  if (!g_exception_init || !g_bitmap_create || !g_argb8888 || !g_bytes || !g_object) {
    return JNI_ERR;
  }
  return JNI_VERSION_1_6;
}

/* ---- Helpers ------------------------------------------------------------- */

static void throw_status(JNIEnv *env, bdf_status st, const char *message) {
  if ((*env)->ExceptionCheck(env)) {
    return; /* one already thrown (out of memory, say) */
  }
  /* libbdf's messages are static ASCII strings */
  jstring m = (*env)->NewStringUTF(env, message && *message ? message : "libbdf failed");
  jobject e = m ? (*env)->NewObject(env, g_exception, g_exception_init, (jint)st, m) : NULL;
  if (e) {
    (*env)->Throw(env, (jthrowable)e);
  }
}

static void throw_error(JNIEnv *env, bdf_status st, const bdf_error *err) {
  throw_status(env, st, err->message);
}

static jbyteArray bytes_of(JNIEnv *env, const void *p, size_t n) {
  if (n > INT32_MAX) {
    throw_status(env, BDF_ERR_NOMEM, "a string too long for Java");
    return NULL;
  }
  jbyteArray a = (*env)->NewByteArray(env, (jsize)n);
  if (a && n > 0) {
    (*env)->SetByteArrayRegion(env, a, 0, (jsize)n, (const jbyte *)p);
  }
  return a;
}

/* A copy of a byte[] with a NUL after it; *n its length. */
static char *copy_bytes(JNIEnv *env, jbyteArray a, size_t *n) {
  jsize len = a ? (*env)->GetArrayLength(env, a) : 0;
  char *p = malloc((size_t)len + 1);
  if (!p) {
    throw_status(env, BDF_ERR_NOMEM, "out of memory");
    return NULL;
  }
  if (len > 0) {
    (*env)->GetByteArrayRegion(env, a, 0, len, (jbyte *)p);
  }
  p[len] = '\0';
  *n = (size_t)len;
  return p;
}

static void wipe(void *p, size_t n) {
  volatile unsigned char *v = p;
  while (n--) {
    *v++ = 0;
  }
}

static jobjectArray byte_arrays(JNIEnv *env, size_t n) {
  return (*env)->NewObjectArray(env, (jsize)n, g_bytes, NULL);
}

static int set_bytes(JNIEnv *env, jobjectArray a, size_t i, const void *p, size_t n) {
  jbyteArray b = bytes_of(env, p, n);
  if (!b) {
    return -1;
  }
  (*env)->SetObjectArrayElement(env, a, (jsize)i, b);
  (*env)->DeleteLocalRef(env, b);
  return 0;
}

/* ---- Documents ----------------------------------------------------------- */

/* A document and what it reads from: bytes it owns, or a file descriptor. */
typedef struct jdoc {
  bdf_doc *doc;
  uint8_t *data;
  int fd;
} jdoc;

static int fd_read_at(void *ctx, uint64_t off, uint8_t *buf, size_t len) {
  const jdoc *d = ctx;
  for (size_t done = 0; done < len;) {
    ssize_t n = pread64(d->fd, buf + done, len - done, (off64_t)(off + done));
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      return -1;
    }
    done += (size_t)n;
  }
  return 0;
}

static void doc_free(jdoc *d) {
  if (!d) {
    return;
  }
  bdf_doc_free(d->doc);
  free(d->data);
  if (d->fd >= 0) {
    close(d->fd);
  }
  free(d);
}

static jlong doc_finish(JNIEnv *env, jdoc *d, bdf_status st, const bdf_error *err) {
  if (st != BDF_OK) {
    throw_error(env, st, err);
    doc_free(d);
    return 0;
  }
  return (jlong)(intptr_t)d;
}

JNIEXPORT jlong JNICALL FN(openBytes)(JNIEnv *env, jclass cls, jbyteArray data) {
  (void)cls;
  jdoc *d = calloc(1, sizeof *d);
  size_t n = 0;
  if (!d) {
    throw_status(env, BDF_ERR_NOMEM, "out of memory");
    return 0;
  }
  d->fd = -1;
  d->data = (uint8_t *)copy_bytes(env, data, &n);
  if (!d->data) {
    doc_free(d);
    return 0;
  }
  bdf_error err;
  bdf_status st = bdf_doc_open_memory(d->data, n, &d->doc, &err);
  return doc_finish(env, d, st, &err);
}

/* A file the document is read from where the reader asks, so that a large
 * one is never read whole: the descriptor is the document's own copy. */
JNIEXPORT jlong JNICALL FN(openFd)(JNIEnv *env, jclass cls, jint fd) {
  (void)cls;
  jdoc *d = calloc(1, sizeof *d);
  if (!d) {
    throw_status(env, BDF_ERR_NOMEM, "out of memory");
    return 0;
  }
  d->fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
  struct stat st_;
  if (d->fd < 0 || fstat(d->fd, &st_) != 0 || st_.st_size < 0) {
    doc_free(d);
    throw_status(env, BDF_ERR_ARG, "the file cannot be read");
    return 0;
  }
  bdf_source src;
  memset(&src, 0, sizeof src);
  src.ctx = d;
  src.read_at = fd_read_at;
  src.size = (uint64_t)st_.st_size;
  bdf_error err;
  bdf_status st = bdf_doc_open(&src, &d->doc, &err);
  return doc_finish(env, d, st, &err);
}

JNIEXPORT void JNICALL FN(closeDocument)(JNIEnv *env, jclass cls, jlong h) {
  (void)env;
  (void)cls;
  doc_free((jdoc *)(intptr_t)h);
}

static bdf_doc *doc_of(jlong h) { return ((jdoc *)(intptr_t)h)->doc; }

JNIEXPORT jboolean JNICALL FN(encrypted)(JNIEnv *env, jclass cls, jlong h) {
  (void)env;
  (void)cls;
  return bdf_doc_encrypted(doc_of(h)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL FN(locked)(JNIEnv *env, jclass cls, jlong h) {
  (void)env;
  (void)cls;
  return bdf_doc_locked(doc_of(h)) ? JNI_TRUE : JNI_FALSE;
}

/* The password comes as its UTF-8; libbdf puts it in form KC. */
JNIEXPORT void JNICALL FN(unlock)(JNIEnv *env, jclass cls, jlong h, jbyteArray password) {
  (void)cls;
  size_t n;
  char *pw = copy_bytes(env, password, &n);
  if (!pw) {
    return;
  }
  bdf_error err;
  bdf_status st = bdf_doc_unlock(doc_of(h), pw, n, &err);
  wipe(pw, n);
  free(pw);
  if (st != BDF_OK) {
    throw_error(env, st, &err);
  }
}

JNIEXPORT jint JNICALL FN(viewCount)(JNIEnv *env, jclass cls, jlong h) {
  (void)env;
  (void)cls;
  return (jint)bdf_doc_view_count(doc_of(h));
}

/* The numbers of a view: kind, pages, right to left, text index, columns
 * and rows (their runs), or null. */
JNIEXPORT jlongArray JNICALL FN(viewNumbers)(JNIEnv *env, jclass cls, jlong h, jint i) {
  (void)cls;
  bdf_view v;
  if (i < 0 || bdf_doc_view(doc_of(h), (size_t)i, &v) != BDF_OK) {
    return NULL;
  }
  jlong n[6] = {v.kind_code, (jlong)v.npages, v.rtl, v.text_index_len > 0, (jlong)v.ncols,
                (jlong)v.nrows};
  jlongArray a = (*env)->NewLongArray(env, 6);
  if (a) {
    (*env)->SetLongArrayRegion(env, a, 0, 6, n);
  }
  return a;
}

/* The strings of a view: id, title, kind as written, direction. */
JNIEXPORT jobjectArray JNICALL FN(viewStrings)(JNIEnv *env, jclass cls, jlong h, jint i) {
  (void)cls;
  bdf_view v;
  if (i < 0 || bdf_doc_view(doc_of(h), (size_t)i, &v) != BDF_OK) {
    return NULL;
  }
  jobjectArray a = byte_arrays(env, 4);
  if (!a || set_bytes(env, a, 0, v.id, v.id_len) < 0 || set_bytes(env, a, 1, v.title, v.title_len) < 0 ||
      set_bytes(env, a, 2, v.kind, v.kind_len) < 0 ||
      set_bytes(env, a, 3, v.direction, v.direction_len) < 0) {
    return NULL;
  }
  return a;
}

JNIEXPORT jfloatArray JNICALL FN(pageSize)(JNIEnv *env, jclass cls, jlong h, jint view, jint page) {
  (void)cls;
  bdf_page p;
  if (view < 0 || page < 0 || bdf_doc_page(doc_of(h), (size_t)view, (size_t)page, &p) != BDF_OK) {
    return NULL;
  }
  jfloat wh[2] = {p.w, p.h};
  jfloatArray a = (*env)->NewFloatArray(env, 2);
  if (a) {
    (*env)->SetFloatArrayRegion(env, a, 0, 2, wh);
  }
  return a;
}

JNIEXPORT jdoubleArray JNICALL FN(sheetSize)(JNIEnv *env, jclass cls, jlong h, jint view) {
  (void)cls;
  jdouble wh[2] = {0, 0};
  if (view >= 0) {
    bdf_sheet_size(doc_of(h), (size_t)view, &wh[0], &wh[1]);
  }
  jdoubleArray a = (*env)->NewDoubleArray(env, 2);
  if (a) {
    (*env)->SetDoubleArrayRegion(env, a, 0, 2, wh);
  }
  return a;
}

/* The values of a Dublin Core element (title, creator, ...). */
JNIEXPORT jobjectArray JNICALL FN(dublinCore)(JNIEnv *env, jclass cls, jlong h, jbyteArray element) {
  (void)cls;
  size_t n;
  char *name = copy_bytes(env, element, &n);
  if (!name) {
    return NULL;
  }
  const bdf_doc *doc = doc_of(h);
  size_t count = bdf_doc_dc_count(doc, name);
  jobjectArray a = byte_arrays(env, count);
  for (size_t i = 0; a && i < count; i++) {
    size_t len;
    const char *v = bdf_doc_dc(doc, name, i, &len);
    if (set_bytes(env, a, i, v, len) < 0) {
      a = NULL;
    }
  }
  free(name);
  return a;
}

/* What made the document: its source format and the generator. */
JNIEXPORT jobjectArray JNICALL FN(meta)(JNIEnv *env, jclass cls, jlong h) {
  (void)cls;
  bdf_meta m;
  bdf_doc_meta(doc_of(h), &m);
  jobjectArray a = byte_arrays(env, 2);
  if (!a || set_bytes(env, a, 0, m.source, m.source_len) < 0 ||
      set_bytes(env, a, 1, m.generator, m.generator_len) < 0) {
    return NULL;
  }
  return a;
}

/* ---- Text ---------------------------------------------------------------- */

/* The text of a view in reading order, joined as its runs say. */
JNIEXPORT jbyteArray JNICALL FN(viewText)(JNIEnv *env, jclass cls, jlong h, jint view) {
  (void)cls;
  bdf_text_index *index = NULL;
  char *text = NULL;
  size_t len = 0;
  bdf_error err;
  bdf_status st = view < 0 ? BDF_ERR_ARG : bdf_doc_view_text(doc_of(h), (size_t)view, 0, &index, &err);
  if (st == BDF_OK) {
    st = bdf_text_index_plain(index, &text, &len, &err);
  } else if (view < 0) {
    err.message = "no such view";
  }
  jbyteArray out = NULL;
  if (st != BDF_OK) {
    throw_error(env, st, &err);
  } else {
    out = bytes_of(env, text, len);
  }
  bdf_free(text);
  bdf_text_index_free(index);
  return out;
}

typedef struct jsearch {
  bdf_text_index *index;
  bdf_text_search *search;
} jsearch;

static void search_free(jsearch *s) {
  if (s) {
    bdf_text_search_free(s->search); /* before the index it reads */
    bdf_text_index_free(s->index);
    free(s);
  }
}

JNIEXPORT jlong JNICALL FN(searchNew)(JNIEnv *env, jclass cls, jlong h, jint view,
                                      jboolean case_sensitive) {
  (void)cls;
  jsearch *s = calloc(1, sizeof *s);
  if (!s) {
    throw_status(env, BDF_ERR_NOMEM, "out of memory");
    return 0;
  }
  bdf_error err;
  bdf_status st = view < 0 ? BDF_ERR_ARG : bdf_doc_view_text(doc_of(h), (size_t)view, 0, &s->index, &err);
  if (st == BDF_OK) {
    st = bdf_text_search_new(s->index, case_sensitive ? 1 : 0, &s->search, &err);
  } else if (view < 0) {
    err.message = "no such view";
  }
  if (st != BDF_OK) {
    throw_error(env, st, &err);
    search_free(s);
    return 0;
  }
  return (jlong)(intptr_t)s;
}

JNIEXPORT void JNICALL FN(searchFree)(JNIEnv *env, jclass cls, jlong h) {
  (void)env;
  (void)cls;
  search_free((jsearch *)(intptr_t)h);
}

/*
 * The hits of a query: [texts, contexts, where], the first two byte[][] and
 * the last an int[] of five a hit: the page (or tile x), the layer (or tile
 * y), and the run, start and end in bytes of the first run it lies in.
 */
JNIEXPORT jobjectArray JNICALL FN(searchFind)(JNIEnv *env, jclass cls, jlong h, jbyteArray query,
                                              jint limit, jint context) {
  (void)cls;
  const jsearch *s = (const jsearch *)(intptr_t)h;
  size_t qn;
  char *q = copy_bytes(env, query, &qn);
  if (!q) {
    return NULL;
  }
  bdf_text_hits *hits = NULL;
  bdf_error err;
  bdf_status st = bdf_text_search_find(s->search, q, qn, limit < 0 ? 0 : (size_t)limit,
                                       context < 0 ? 0 : (size_t)context, &hits, &err);
  free(q);
  if (st != BDF_OK) {
    throw_error(env, st, &err);
    return NULL;
  }
  size_t n = bdf_text_hits_count(hits);
  jobjectArray texts = byte_arrays(env, n), contexts = byte_arrays(env, n);
  jint *where = calloc(n * 5 + 1, sizeof *where);
  jobjectArray out = NULL;
  if (texts && contexts && where) {
    size_t i = 0;
    for (; i < n; i++) {
      bdf_text_hit hit;
      bdf_text_hit_segment seg;
      if (bdf_text_hits_at(hits, i, &hit) != BDF_OK ||
          bdf_text_hits_segment(hits, i, 0, &seg) != BDF_OK ||
          set_bytes(env, texts, i, hit.text, hit.text_len) < 0 ||
          set_bytes(env, contexts, i, hit.context, hit.context_len) < 0) {
        break;
      }
      jint *w = where + 5 * i;
      w[0] = (jint)seg.a;
      w[1] = (jint)seg.b;
      w[2] = (jint)seg.run;
      w[3] = (jint)seg.start;
      w[4] = (jint)seg.end;
    }
    jintArray wa = i == n ? (*env)->NewIntArray(env, (jsize)(5 * n)) : NULL;
    out = wa ? (*env)->NewObjectArray(env, 3, g_object, NULL) : NULL;
    if (out) {
      (*env)->SetIntArrayRegion(env, wa, 0, (jsize)(5 * n), where);
      (*env)->SetObjectArrayElement(env, out, 0, texts);
      (*env)->SetObjectArrayElement(env, out, 1, contexts);
      (*env)->SetObjectArrayElement(env, out, 2, wa);
    }
  } else if (!where) {
    throw_status(env, BDF_ERR_NOMEM, "out of memory");
  }
  free(where);
  bdf_text_hits_free(hits);
  return out;
}

/* ---- Fonts --------------------------------------------------------------- */

JNIEXPORT jlong JNICALL FN(fontsNew)(JNIEnv *env, jclass cls) {
  (void)cls;
  bdf_fonts *f = NULL;
  bdf_error err;
  bdf_status st = bdf_fonts_new(&f, &err);
  if (st != BDF_OK) {
    throw_error(env, st, &err);
    return 0;
  }
  return (jlong)(intptr_t)f;
}

JNIEXPORT void JNICALL FN(fontsFree)(JNIEnv *env, jclass cls, jlong h) {
  (void)env;
  (void)cls;
  bdf_fonts_free((bdf_fonts *)(intptr_t)h);
}

JNIEXPORT void JNICALL FN(fontsAddFile)(JNIEnv *env, jclass cls, jlong h, jbyteArray path) {
  (void)cls;
  size_t n;
  char *p = copy_bytes(env, path, &n);
  if (!p) {
    return;
  }
  bdf_error err;
  bdf_status st = bdf_fonts_add_file((bdf_fonts *)(intptr_t)h, p, &err);
  free(p);
  if (st != BDF_OK) {
    throw_error(env, st, &err);
  }
}

JNIEXPORT void JNICALL FN(fontsAdd)(JNIEnv *env, jclass cls, jlong h, jbyteArray data) {
  (void)cls;
  size_t n;
  char *p = copy_bytes(env, data, &n);
  if (!p) {
    return;
  }
  bdf_error err;
  bdf_status st = bdf_fonts_add((bdf_fonts *)(intptr_t)h, (const uint8_t *)p, n, &err);
  free(p); /* the set keeps a copy */
  if (st != BDF_OK) {
    throw_error(env, st, &err);
  }
}

JNIEXPORT jint JNICALL FN(fontsCount)(JNIEnv *env, jclass cls, jlong h) {
  (void)env;
  (void)cls;
  return (jint)bdf_fonts_count((const bdf_fonts *)(intptr_t)h);
}

/* ---- Rendering ----------------------------------------------------------- */

JNIEXPORT jlong JNICALL FN(rendererNew)(JNIEnv *env, jclass cls, jlong doc, jlong fonts,
                                        jboolean has_background, jint background) {
  (void)cls;
  bdf_render_options opts;
  memset(&opts, 0, sizeof opts);
  opts.fonts = (const bdf_fonts *)(intptr_t)fonts;
  opts.has_background = has_background ? 1 : 0;
  opts.background = (uint32_t)background;
  bdf_renderer *r = NULL;
  bdf_error err;
  bdf_status st = bdf_renderer_new(doc_of(doc), &opts, &r, &err);
  if (st != BDF_OK) {
    throw_error(env, st, &err);
    return 0;
  }
  return (jlong)(intptr_t)r;
}

JNIEXPORT void JNICALL FN(rendererFree)(JNIEnv *env, jclass cls, jlong h) {
  (void)env;
  (void)cls;
  bdf_renderer_free((bdf_renderer *)(intptr_t)h);
}

/* Copies an image libbdf drew into a bitmap of its size. libbdf's pixels
 * are red, green, blue and alpha in memory, premultiplied: what an
 * ARGB_8888 bitmap holds, so the rows are copied as they are. */
static int copy_into(JNIEnv *env, const bdf_image *img, jobject bitmap) {
  AndroidBitmapInfo info;
  void *pixels = NULL;
  if (AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS ||
      info.format != ANDROID_BITMAP_FORMAT_RGBA_8888 || (int)info.width != img->width ||
      (int)info.height != img->height) {
    throw_status(env, BDF_ERR_ARG, "the bitmap is not an ARGB_8888 bitmap of the size asked for");
    return -1;
  }
  if (AndroidBitmap_lockPixels(env, bitmap, &pixels) != ANDROID_BITMAP_RESULT_SUCCESS) {
    throw_status(env, BDF_ERR_ARG, "the bitmap cannot be written");
    return -1;
  }
  size_t row = (size_t)img->width * 4;
  for (int y = 0; y < img->height; y++) {
    memcpy((uint8_t *)pixels + (size_t)y * info.stride, img->pixels + (size_t)y * row, row);
  }
  AndroidBitmap_unlockPixels(env, bitmap);
  return 0;
}

/* A whole page (or a strip of a scroll view) at scale pixels a unit, as a
 * new bitmap. */
JNIEXPORT jobject JNICALL FN(renderPage)(JNIEnv *env, jclass cls, jlong h, jint view, jint page,
                                         jdouble scale) {
  (void)cls;
  if (view < 0 || page < 0) {
    throw_status(env, BDF_ERR_ARG, "no such page");
    return NULL;
  }
  bdf_image img;
  bdf_error err;
  bdf_status st = bdf_render_page((bdf_renderer *)(intptr_t)h, (size_t)view, (size_t)page, scale,
                                  &img, &err);
  if (st != BDF_OK) {
    throw_error(env, st, &err);
    return NULL;
  }
  jobject bitmap = (*env)->CallStaticObjectMethod(env, g_bitmap, g_bitmap_create, (jint)img.width,
                                                  (jint)img.height, g_argb8888);
  if (bitmap && copy_into(env, &img, bitmap) < 0) {
    bitmap = NULL;
  }
  bdf_image_free(&img);
  return bitmap;
}

/* A rectangle of a view (units) drawn into the whole of a bitmap: of a page,
 * of the strips of a scroll view stacked, or of a sheet. */
JNIEXPORT void JNICALL FN(renderRegion)(JNIEnv *env, jclass cls, jlong h, jint view, jint page,
                                        jfloat x, jfloat y, jfloat w, jfloat height, jobject bitmap) {
  (void)cls;
  AndroidBitmapInfo info;
  if (view < 0 || page < 0 ||
      AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS) {
    throw_status(env, BDF_ERR_ARG, "bad argument");
    return;
  }
  const float region[4] = {x, y, w, height};
  bdf_image img;
  bdf_error err;
  bdf_status st = bdf_render_region((bdf_renderer *)(intptr_t)h, (size_t)view, (size_t)page, region,
                                    (int)info.width, (int)info.height, &img, &err);
  if (st != BDF_OK) {
    throw_error(env, st, &err);
    return;
  }
  copy_into(env, &img, bitmap);
  bdf_image_free(&img);
}

JNIEXPORT jobjectArray JNICALL FN(warnings)(JNIEnv *env, jclass cls, jlong h) {
  (void)cls;
  const bdf_renderer *r = (const bdf_renderer *)(intptr_t)h;
  size_t n = bdf_renderer_warning_count(r);
  jobjectArray a = byte_arrays(env, n);
  for (size_t i = 0; a && i < n; i++) {
    const char *w = bdf_renderer_warning(r, i);
    if (set_bytes(env, a, i, w, strlen(w)) < 0) {
      a = NULL;
    }
  }
  return a;
}

JNIEXPORT jbyteArray JNICALL FN(version)(JNIEnv *env, jclass cls) {
  (void)cls;
  const char *v = bdf_version();
  return bytes_of(env, v, strlen(v));
}
