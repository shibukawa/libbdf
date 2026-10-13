/*
 * The images of the rasterizer: an image part decoded into a picture as the
 * reference decodes it (image.go of raster/imagebdf, with Sniff, Orientation
 * and Orient of image/imgconv), the smaller copies a picture is sampled
 * from when it is drawn small, and the shaders that draw images and
 * patterns (paint.go).
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "raster_internal.h"

/* ---- Formats ------------------------------------------------------------- */

static int bdfi_has(const uint8_t *b, size_t n, size_t at, const char *s) {
  size_t m = strlen(s);
  return n >= at + m && memcmp(b + at, s, m) == 0;
}

/* imgconv.Sniff: the format of encoded image bytes. */
BDF_INTERNAL int bdfi_image_sniff(const uint8_t *b, size_t n) {
  if (n >= 8 && memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0) {
    return BDFI_IMAGE_PNG;
  }
  if (n >= 3 && b[0] == 0xff && b[1] == 0xd8 && b[2] == 0xff) {
    return BDFI_IMAGE_JPEG;
  }
  if (n >= 12 && bdfi_has(b, n, 0, "RIFF") && bdfi_has(b, n, 8, "WEBP")) {
    return BDFI_IMAGE_WEBP;
  }
  if (n >= 12 && bdfi_has(b, n, 4, "ftyp") &&
      (bdfi_has(b, n, 8, "avif") || bdfi_has(b, n, 8, "avis"))) {
    return BDFI_IMAGE_AVIF;
  }
  if (bdfi_has(b, n, 0, "GIF87a") || bdfi_has(b, n, 0, "GIF89a")) {
    return BDFI_IMAGE_GIF;
  }
  if (n >= 2 && b[0] == 'B' && b[1] == 'M') {
    return BDFI_IMAGE_BMP;
  }
  size_t head = n < 512 ? n : 512;
  for (size_t i = 0; i + 4 <= head; i++) {
    if (memcmp(b + i, "<svg", 4) == 0) {
      return BDFI_IMAGE_SVG;
    }
  }
  return BDFI_IMAGE_NONE;
}

BDF_INTERNAL const char *bdfi_image_name(int format) {
  static const char *const names[] = {"", "png", "jpeg", "webp", "avif", "gif", "bmp", "svg"};
  return names[format];
}

/* isSVG in image.go: whether image bytes are SVG markup (after a byte order
 * mark and white space) rather than an encoded bitmap. */
BDF_INTERNAL int bdfi_image_is_svg(const uint8_t *b, size_t n) {
  if (n >= 2 && ((b[0] == 0xff && b[1] == 0xfe) || (b[0] == 0xfe && b[1] == 0xff))) {
    return 1; /* UTF-16 */
  }
  if (n >= 3 && b[0] == 0xef && b[1] == 0xbb && b[2] == 0xbf) {
    b += 3;
    n -= 3;
  }
  while (n > 0 && (*b == ' ' || *b == '\t' || *b == '\r' || *b == '\n')) {
    b++;
    n--;
  }
  return n > 0 && *b == '<';
}

/*
 * image.DecodeConfig of the reference: the size of an image by the decoder
 * its first bytes name. (The names differ a little from those of Sniff: a
 * '?' matches any byte.)
 */
BDF_INTERNAL int bdfi_image_config(const uint8_t *b, size_t n, int32_t *w, int32_t *h) {
  *w = 0;
  *h = 0;
  if (n >= 8 && memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0) {
    return bdfi_png_config(b, n, w, h);
  }
  if (n >= 2 && b[0] == 0xff && b[1] == 0xd8) {
    return bdfi_jpeg_config(b, n, w, h);
  }
  if (n >= 6 && bdfi_has(b, n, 0, "GIF8") && b[5] == 'a') {
    return bdfi_gif_config(b, n, w, h);
  }
  if (n >= 10 && b[0] == 'B' && b[1] == 'M' && b[6] == 0 && b[7] == 0 && b[8] == 0 && b[9] == 0) {
    return bdfi_bmp_config(b, n, w, h);
  }
  if (n >= 15 && bdfi_has(b, n, 0, "RIFF") && bdfi_has(b, n, 8, "WEBPVP8")) {
    return bdfi_webp_config(b, n, w, h);
  }
  return 0;
}

/* ---- Orientation --------------------------------------------------------- */

static uint32_t bdfi_exif16(const uint8_t *p, int little) {
  return little ? (uint32_t)(p[0] | p[1] << 8) : (uint32_t)(p[0] << 8 | p[1]);
}

static uint32_t bdfi_exif32(const uint8_t *p, int little) {
  return little ? bdfi_exif16(p, 1) | bdfi_exif16(p + 2, 1) << 16
                : bdfi_exif16(p, 0) << 16 | bdfi_exif16(p + 2, 0);
}

/*
 * imgconv.Orientation: the EXIF orientation (1 to 8) that browsers apply
 * when they draw a JPEG or PNG image, from a JPEG APP1 segment or a PNG
 * eXIf chunk, or 1 when there is none. WebP images are drawn as stored.
 */
BDF_INTERNAL int bdfi_image_orientation(const uint8_t *data, size_t len) {
  const uint8_t *exif = NULL;
  size_t n = 0;
  int format = bdfi_image_sniff(data, len);
  if (format == BDFI_IMAGE_JPEG) {
    for (size_t pos = 2; pos + 4 <= len && data[pos] == 0xff;) {
      uint8_t marker = data[pos + 1];
      if (marker == 0xda || marker == 0xd9) { /* start of scan, end of image */
        break;
      }
      size_t seg = (size_t)data[pos + 2] << 8 | data[pos + 3];
      if (seg < 2 || seg > len - pos - 2) {
        break;
      }
      if (marker == 0xe1 && bdfi_has(data + pos + 4, seg - 2, 0, "Exif") && seg - 2 >= 6 &&
          data[pos + 8] == 0 && data[pos + 9] == 0) {
        exif = data + pos + 10;
        n = seg - 8;
        break;
      }
      pos += 2 + seg;
    }
  } else if (format == BDFI_IMAGE_PNG) {
    for (size_t pos = 8; pos + 12 <= len;) {
      size_t chunk = (size_t)data[pos] << 24 | (size_t)data[pos + 1] << 16 |
                     (size_t)data[pos + 2] << 8 | data[pos + 3];
      if (chunk > len - pos - 12) {
        break;
      }
      if (memcmp(data + pos + 4, "eXIf", 4) == 0) {
        exif = data + pos + 8;
        n = chunk;
        if (n >= 6 && memcmp(exif, "Exif\0\0", 6) == 0) {
          exif += 6;
          n -= 6;
        }
        break;
      }
      pos += 12 + chunk;
    }
  }
  if (n < 8) {
    return 1;
  }
  int little;
  if (memcmp(exif, "II*\0", 4) == 0) {
    little = 1;
  } else if (memcmp(exif, "MM\0*", 4) == 0) {
    little = 0;
  } else {
    return 1;
  }
  size_t ifd = bdfi_exif32(exif + 4, little);
  if (ifd > n || n - ifd < 2) {
    return 1;
  }
  size_t count = bdfi_exif16(exif + ifd, little);
  for (size_t i = 0; i < count; i++) {
    size_t e = ifd + 2 + 12 * i;
    if (e > n || n - e < 12) {
      break;
    }
    if (bdfi_exif16(exif + e, little) == 0x0112 && bdfi_exif16(exif + e + 2, little) == 3) {
      uint32_t o = bdfi_exif16(exif + e + 8, little); /* Orientation, a SHORT */
      if (o >= 1 && o <= 8) {
        return (int)o;
      }
      break;
    }
  }
  return 1;
}

/* imgconv.Orient: an image turned as its EXIF orientation says it is
 * displayed. It replaces the bitmap; -1 when out of memory. */
static int bdfi_image_orient(bdfi_bitmap *img, int o) {
  if (o <= 1 || o > 8) {
    return 0;
  }
  size_t w = (size_t)img->w, h = (size_t)img->h;
  size_t ow = o >= 5 ? h : w, oh = o >= 5 ? w : h;
  uint8_t *dst = bdfi_malloc_array(ow * oh, 4);
  if (!dst) {
    return -1;
  }
  for (size_t y = 0; y < oh; y++) {
    for (size_t x = 0; x < ow; x++) {
      size_t sx = 0, sy = 0;
      switch (o) {
      case 2:
        sx = w - 1 - x;
        sy = y;
        break;
      case 3:
        sx = w - 1 - x;
        sy = h - 1 - y;
        break;
      case 4:
        sx = x;
        sy = h - 1 - y;
        break;
      case 5:
        sx = y;
        sy = x;
        break;
      case 6:
        sx = y;
        sy = h - 1 - x;
        break;
      case 7:
        sx = w - 1 - y;
        sy = h - 1 - x;
        break;
      default:
        sx = w - 1 - y;
        sy = x;
        break;
      }
      memcpy(dst + 4 * (y * ow + x), img->pix + 4 * (sy * w + sx), 4);
    }
  }
  free(img->pix);
  img->pix = dst;
  img->w = (int32_t)ow;
  img->h = (int32_t)oh;
  return 0;
}

/* ---- Pictures ------------------------------------------------------------ */

BDF_INTERNAL void bdfi_picture_free(bdfi_picture *p) {
  if (!p) {
    return;
  }
  for (int i = 0; i < p->nlevels; i++) {
    free(p->levels[i].pix);
  }
  free(p);
}

/*
 * decodePicture: a bitmap image part as a picture. The status is BDF_OK,
 * BDF_ERR_TOO_LARGE (more than BDFI_MAX_DECODE_PIXELS pixels: four bytes a
 * pixel, more memory than a preview is worth), BDF_ERR_NOMEM, or
 * BDF_ERR_FORMAT for what is not decoded (*format says what it is).
 */
BDF_INTERNAL bdf_status bdfi_picture_decode(const uint8_t *data, size_t len, bdfi_picture **out,
                                            int *format) {
  *out = NULL;
  *format = bdfi_image_sniff(data, len);
  bdfi_bitmap bm;
  bdf_status st;
  switch (*format) {
  case BDFI_IMAGE_PNG:
    st = bdfi_png_decode(data, len, &bm);
    break;
  case BDFI_IMAGE_JPEG:
    st = bdfi_jpeg_decode(data, len, &bm);
    break;
  case BDFI_IMAGE_GIF:
    st = bdfi_gif_decode(data, len, &bm);
    break;
  case BDFI_IMAGE_BMP:
    st = bdfi_bmp_decode(data, len, &bm);
    break;
  case BDFI_IMAGE_WEBP:
    st = bdfi_webp_decode(data, len, &bm);
    break;
  default:
    return BDF_ERR_FORMAT;
  }
  if (st != BDF_OK) {
    return st;
  }
  /* browsers turn JPEG and PNG images by their EXIF orientation */
  bdfi_picture *p = calloc(1, sizeof *p);
  if (!p || bdfi_image_orient(&bm, bdfi_image_orientation(data, len)) < 0) {
    free(bm.pix);
    free(p);
    return BDF_ERR_NOMEM;
  }
  p->w = bm.w;
  p->h = bm.h;
  p->levels[0] = bm;
  p->nlevels = 1;
  *out = p;
  return BDF_OK;
}

/* halve: an image scaled down by two with a box filter. */
static int bdfi_picture_halve(const bdfi_bitmap *src, bdfi_bitmap *dst) {
  size_t sw = (size_t)src->w, sh = (size_t)src->h;
  size_t w = (sw + 1) / 2, h = (sh + 1) / 2;
  dst->pix = bdfi_malloc_array(w * h, 4);
  if (!dst->pix) {
    return -1;
  }
  dst->w = (int32_t)w;
  dst->h = (int32_t)h;
  for (size_t y = 0; y < h; y++) {
    for (size_t x = 0; x < w; x++) {
      unsigned sum[4] = {0, 0, 0, 0}, n = 0;
      for (size_t dy = 0; dy < 2; dy++) {
        for (size_t dx = 0; dx < 2; dx++) {
          size_t sx = 2 * x + dx, sy = 2 * y + dy;
          if (sx >= sw || sy >= sh) {
            continue;
          }
          const uint8_t *s = src->pix + 4 * (sy * sw + sx);
          for (int c = 0; c < 4; c++) {
            sum[c] += s[c];
          }
          n++;
        }
      }
      uint8_t *o = dst->pix + 4 * (y * w + x);
      for (int c = 0; c < 4; c++) {
        o[c] = (uint8_t)((sum[c] + n / 2) / n);
      }
    }
  }
  return 0;
}

/*
 * levelFor: the copy of a picture to sample through inv (device to image
 * pixels), the one where a device pixel spans at most about two image
 * pixels. The copies are made as they are asked for; when memory runs out
 * (*oom) the smallest there is has to do.
 */
BDF_INTERNAL int bdfi_picture_level(bdfi_picture *p, const bdfi_mat *inv, int *oom) {
  double f = bdfi_mat_max_scale(inv);
  if (!(f > 2)) {
    return 0;
  }
  /* bounded as a double: a scale no int holds is that of an image drawn in
   * no place at all */
  int l = (int)bdfi_fmin(floor(bdfi_log2(f)), BDFI_MAX_LEVELS - 1);
  while (p->nlevels <= l) {
    const bdfi_bitmap *prev = &p->levels[p->nlevels - 1];
    if (prev->w <= 1 && prev->h <= 1) {
      return p->nlevels - 1;
    }
    if (bdfi_picture_halve(prev, &p->levels[p->nlevels]) < 0) {
      *oom = 1;
      return p->nlevels - 1;
    }
    p->nlevels++;
  }
  return l;
}

static void bdfi_picture_at(const bdfi_bitmap *img, int64_t px, int64_t py, int wrap_x, int wrap_y,
                            float out[4]) {
  int64_t w = img->w, h = img->h;
  if (wrap_x) {
    px = ((px % w) + w) % w;
  } else {
    px = px < 0 ? 0 : px > w - 1 ? w - 1 : px;
  }
  if (wrap_y) {
    py = ((py % h) + h) % h;
  } else {
    py = py < 0 ? 0 : py > h - 1 ? h - 1 : py;
  }
  const uint8_t *s = img->pix + 4 * ((size_t)py * (size_t)w + (size_t)px);
  for (int k = 0; k < 4; k++) {
    out[k] = (float)s[k] / 255;
  }
}

/*
 * sample: the colour at (x, y) in pixels of the full image, read from one
 * of its copies and interpolated between four pixels; wrap_x and wrap_y
 * repeat the image instead of holding its edges.
 */
BDF_INTERNAL void bdfi_picture_sample(const bdfi_picture *p, int level, double x, double y,
                                      int wrap_x, int wrap_y, float out[4]) {
  const bdfi_bitmap *img = &p->levels[level < p->nlevels ? level : p->nlevels - 1];
  x = x * (double)img->w / (double)p->w - 0.5;
  y = y * (double)img->h / (double)p->h - 0.5;
  double x0 = floor(x), y0 = floor(y);
  float fx = (float)(x - x0), fy = (float)(y - y0);
  int64_t ix = bdfi_to_int(x0), iy = bdfi_to_int(y0);
  float a[4], b[4], c[4], d[4];
  bdfi_picture_at(img, ix, iy, wrap_x, wrap_y, a);
  bdfi_picture_at(img, ix + 1, iy, wrap_x, wrap_y, b);
  bdfi_picture_at(img, ix, iy + 1, wrap_x, wrap_y, c);
  bdfi_picture_at(img, ix + 1, iy + 1, wrap_x, wrap_y, d);
  for (int k = 0; k < 4; k++) {
    float top = a[k] + (b[k] - a[k]) * fx;
    float bot = c[k] + (d[k] - c[k]) * fx;
    out[k] = top + (bot - top) * fy;
  }
}

/* nearest: the pixel under (x, y) of the full image. */
BDF_INTERNAL void bdfi_picture_nearest(const bdfi_picture *p, double x, double y, float out[4]) {
  bdfi_picture_at(&p->levels[0], bdfi_to_int(floor(x)), bdfi_to_int(floor(y)), 0, 0, out);
}

/* ---- Shaders ------------------------------------------------------------- */

static double bdfi_wrap(double v, double n) {
  v = bdfi_mod(v, n);
  if (v < 0) {
    v += n;
  }
  return v;
}

static void bdfi_pattern_shade(const bdfi_shader *sh, int x, int y, int n, float *dst) {
  const bdfi_pattern_shader *p = (const bdfi_pattern_shader *)sh;
  double w = (double)p->img->w, h = (double)p->img->h;
  int rx = p->repeat == BDF_REPEAT || p->repeat == BDF_REPEAT_X;
  int ry = p->repeat == BDF_REPEAT || p->repeat == BDF_REPEAT_Y;
  for (int i = 0; i < n; i++) {
    bdfi_pt q = bdfi_mat_pt(&p->inv, (double)(x + i) + 0.5, (double)y + 0.5);
    float col[4] = {0, 0, 0, 0};
    int in_x = q.x >= 0 && q.x < w, in_y = q.y >= 0 && q.y < h;
    if (rx) {
      q.x = bdfi_wrap(q.x, w);
      in_x = 1;
    }
    if (ry) {
      q.y = bdfi_wrap(q.y, h);
      in_y = 1;
    }
    if (in_x && in_y) {
      bdfi_picture_sample(p->img, p->level, q.x, q.y, rx, ry, col);
    }
    memcpy(dst + 4 * (size_t)i, col, sizeof col);
  }
}

/* newPatternShader: a pattern of an image drawn under m. 0 when it draws
 * nothing (the caller draws transparent black). */
BDF_INTERNAL int bdfi_pattern_shader_init(bdfi_pattern_shader *s, const bdf_paint *paint,
                                          bdfi_picture *img, const bdfi_mat *m, double kx,
                                          double ky, int *oom) {
  const float *pm = paint->matrix;
  bdfi_mat pattern = {
      {(double)pm[0], (double)pm[1], (double)pm[2], (double)pm[3], (double)pm[4], (double)pm[5]}};
  bdfi_mat full = bdfi_mat_mul2(*m, pattern);
  if (kx != 1 || ky != 1) {
    full = bdfi_mat_scale(full, kx, ky);
  }
  if (!img || !bdfi_mat_invert(&full, &s->inv)) {
    return 0;
  }
  s->base.shade = bdfi_pattern_shade;
  s->base.flat = 0;
  s->img = img;
  s->repeat = paint->repeat;
  s->level = bdfi_picture_level(img, &s->inv, oom);
  return 1;
}

static void bdfi_image_shade(const bdfi_shader *sh, int x, int y, int n, float *dst) {
  const bdfi_image_shader *s = (const bdfi_image_shader *)sh;
  for (int i = 0; i < n; i++) {
    bdfi_pt q = bdfi_mat_pt(&s->inv, (double)(x + i) + 0.5, (double)y + 0.5);
    double ix = bdfi_fmin(bdfi_fmax(q.x, s->sx0), s->sx1);
    double iy = bdfi_fmin(bdfi_fmax(q.y, s->sy0), s->sy1);
    if (s->smooth || s->level > 0) {
      bdfi_picture_sample(s->img, s->level, ix, iy, 0, 0, dst + 4 * (size_t)i);
    } else {
      bdfi_picture_nearest(s->img, ix, iy, dst + 4 * (size_t)i);
    }
  }
}

/* The shader of an image drawn through inv, sampled from the source
 * rectangle (sx0, sy0)-(sx1, sy1) in image pixels. */
BDF_INTERNAL void bdfi_image_shader_init(bdfi_image_shader *s, bdfi_picture *img,
                                         const bdfi_mat *inv, int smooth, double sx0, double sy0,
                                         double sx1, double sy1, int *oom) {
  s->base.shade = bdfi_image_shade;
  s->base.flat = 0;
  s->inv = *inv;
  s->img = img;
  s->level = bdfi_picture_level(img, inv, oom);
  s->smooth = smooth;
  s->sx0 = sx0;
  s->sy0 = sy0;
  s->sx1 = sx1;
  s->sy1 = sy1;
}
