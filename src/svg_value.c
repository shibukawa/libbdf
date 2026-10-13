/*
 * The values the attributes of an SVG document hold: a port of value.go and
 * color.go of the reference's package image/svg, so that a number, a
 * length, a transform and a colour are the same float64s here as there. The
 * products and sums are those of the Go code in the same order, without
 * fused multiply-adds (raster_internal.h), and the functions of Go's math
 * package are those of rmath.c.
 *
 * Where the Go code makes a lower-case copy of a value first
 * (strings.ToLower, which knows the capital letters of every script), the
 * port reads the value as it is and compares without regard to case: of the
 * characters that are not ASCII only U+0130 and U+212A have ASCII small
 * letters (i and k), and bdfi_svg_lower_ascii knows them.
 */
#include <string.h>

#include "svg_internal.h"

static int bdfi_svg_is_digit(char c) {
  return c >= '0' && c <= '9';
}

/* ParseNumber in value.go. */
int bdfi_svg_parse_number(const char *s, size_t n, double *v, size_t *rest) {
  size_t i = 0;
  while (i < n && (s[i] == ' ' || s[i] == ',' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
    i++;
  }
  size_t j = i;
  if (j < n && (s[j] == '+' || s[j] == '-')) {
    j++;
  }
  int digits = 0, dot = 0;
  while (j < n) {
    if (bdfi_svg_is_digit(s[j])) {
      digits = 1;
    } else if (s[j] == '.' && !dot) {
      dot = 1;
    } else {
      break;
    }
    j++;
  }
  if (!digits) {
    *v = 0;
    *rest = i;
    return 0;
  }
  if (j < n && (s[j] == 'e' || s[j] == 'E')) {
    size_t k = j + 1;
    if (k < n && (s[k] == '+' || s[k] == '-')) {
      k++;
    }
    if (k < n && bdfi_svg_is_digit(s[k])) {
      while (k < n && bdfi_svg_is_digit(s[k])) {
        k++;
      }
      j = k;
    }
  }
  /* a number out of range is an infinity, and no number */
  int err = bdfi_parse_float(s + i, j - i, v);
  *rest = j;
  return err == 0;
}

/* ParseNumbers in value.go. */
size_t bdfi_svg_parse_numbers(const char *s, size_t n, double *out, size_t cap) {
  size_t count = 0, pos = 0;
  for (;;) {
    double v;
    size_t rest;
    if (!bdfi_svg_parse_number(s + pos, n - pos, &v, &rest)) {
      return count;
    }
    if (count < cap) {
      out[count] = v;
    }
    count++;
    pos += rest;
  }
}

/* The sizes of the units in px (units in value.go), as Go rounds its
 * constants: 96/25.4 is the double nearest to the quotient, not the
 * quotient of two doubles. */
#define BDFI_SVG_PX_PT 1.3333333333333333  /* 0x3ff5555555555555: 96/72 */
#define BDFI_SVG_PX_CM 37.795275590551178  /* 0x4042e5cb972e5cb9: 96/2.54 */
#define BDFI_SVG_PX_MM 3.7795275590551181  /* 0x400e3c78f1e3c78f: 96/25.4 */
#define BDFI_SVG_PX_Q  0.94488188976377951 /* 0x3fee3c78f1e3c78f: 96/101.6 */

/* ParseLength in value.go. */
int bdfi_svg_parse_length(const char *s, size_t n, double ref, double em, double *out) {
  double v;
  size_t rest;
  *out = 0;
  if (!bdfi_svg_parse_number(s, n, &v, &rest)) {
    return 0;
  }
  /* u := strings.ToLower(strings.TrimSpace(rest)) */
  char u[2];
  size_t ul =
      bdfi_svg_lower_ascii(bdfi_svg_trim_space(bdfi_svg_str_of(s + rest, n - rest)), u, sizeof u);
  double k;
  if (ul == 0) {
    k = 1;
  } else if (ul == 1 && u[0] == '%') {
    *out = v * ref / 100;
    return 1;
  } else if (ul == 1 && u[0] == 'q') {
    k = BDFI_SVG_PX_Q;
  } else if (ul != 2) {
    return 0;
  } else if (u[0] == 'e' && u[1] == 'm') {
    *out = v * em;
    return 1;
  } else if (u[0] == 'e' && u[1] == 'x') {
    *out = v * em / 2;
    return 1;
  } else if (u[0] == 'p' && u[1] == 'x') {
    k = 1;
  } else if (u[0] == 'p' && u[1] == 't') {
    k = BDFI_SVG_PX_PT;
  } else if (u[0] == 'p' && u[1] == 'c') {
    k = 16;
  } else if (u[0] == 'i' && u[1] == 'n') {
    k = 96;
  } else if (u[0] == 'c' && u[1] == 'm') {
    k = BDFI_SVG_PX_CM;
  } else if (u[0] == 'm' && u[1] == 'm') {
    k = BDFI_SVG_PX_MM;
  } else {
    return 0;
  }
  *out = v * k;
  return 1;
}

/* Matrix.Mul in value.go. */
bdfi_mat bdfi_svg_mat_mul(bdfi_mat m, bdfi_mat n) {
  bdfi_mat r;
  r.v[0] = m.v[0] * n.v[0] + m.v[2] * n.v[1];
  r.v[1] = m.v[1] * n.v[0] + m.v[3] * n.v[1];
  r.v[2] = m.v[0] * n.v[2] + m.v[2] * n.v[3];
  r.v[3] = m.v[1] * n.v[2] + m.v[3] * n.v[3];
  r.v[4] = m.v[0] * n.v[4] + m.v[2] * n.v[5] + m.v[4];
  r.v[5] = m.v[1] * n.v[4] + m.v[3] * n.v[5] + m.v[5];
  return r;
}

static bdfi_mat bdfi_svg_mat_of(double a, double b, double c, double d, double e, double f) {
  bdfi_mat m;
  m.v[0] = a;
  m.v[1] = b;
  m.v[2] = c;
  m.v[3] = d;
  m.v[4] = e;
  m.v[5] = f;
  return m;
}

/* ParseTransform in value.go. */
bdfi_mat bdfi_svg_parse_transform(const char *s, size_t n) {
  bdfi_mat m = bdfi_svg_mat_of(1, 0, 0, 1, 0, 0);
  for (;;) {
    /* s = strings.TrimLeft(s, " ,\t\n\r") */
    while (n > 0 && (*s == ' ' || *s == ',' || *s == '\t' || *s == '\n' || *s == '\r')) {
      s++;
      n--;
    }
    const char *open = n > 0 ? memchr(s, '(', n) : NULL;
    if (!open) {
      return m;
    }
    bdfi_svg_str name = bdfi_svg_trim_space(bdfi_svg_str_of(s, (size_t)(open - s)));
    size_t left = n - (size_t)(open - s);
    const char *close = memchr(open, ')', left);
    if (!close) {
      return m;
    }
    double a[6] = {0, 0, 0, 0, 0, 0};
    size_t na = bdfi_svg_parse_numbers(open + 1, (size_t)(close - open) - 1, a, 6);
    n = left - (size_t)(close - open) - 1;
    s = close + 1;
    bdfi_mat t;
    if (bdfi_svg_str_eq(name, "matrix", 6)) {
      if (na != 6) {
        return m;
      }
      t = bdfi_svg_mat_of(a[0], a[1], a[2], a[3], a[4], a[5]);
    } else if (bdfi_svg_str_eq(name, "translate", 9)) {
      t = bdfi_svg_mat_of(1, 0, 0, 1, a[0], a[1]);
    } else if (bdfi_svg_str_eq(name, "scale", 5)) {
      double sx = na > 0 ? a[0] : 1;
      t = bdfi_svg_mat_of(sx, 0, 0, na > 1 ? a[1] : sx, 0, 0);
    } else if (bdfi_svg_str_eq(name, "rotate", 6)) {
      double sn, cs;
      bdfi_sincos(a[0] * BDFI_PI / 180, &sn, &cs);
      double cx = a[1], cy = a[2];
      t = bdfi_svg_mat_mul(bdfi_svg_mat_mul(bdfi_svg_mat_of(1, 0, 0, 1, cx, cy),
                                            bdfi_svg_mat_of(cs, sn, -sn, cs, 0, 0)),
                           bdfi_svg_mat_of(1, 0, 0, 1, -cx, -cy));
    } else if (bdfi_svg_str_eq(name, "skewX", 5)) {
      t = bdfi_svg_mat_of(1, 0, bdfi_tan(a[0] * BDFI_PI / 180), 1, 0, 0);
    } else if (bdfi_svg_str_eq(name, "skewY", 5)) {
      t = bdfi_svg_mat_of(1, bdfi_tan(a[0] * BDFI_PI / 180), 0, 1, 0, 0);
    } else {
      return m;
    }
    m = bdfi_svg_mat_mul(m, t);
  }
}

/* ---- Colours (color.go, colornames.go) ------------------------------------ */

typedef struct bdfi_svg_named_color {
  char name[21];
  uint32_t rgb;
} bdfi_svg_named_color;

/* namedColors: the CSS named colours, by name. The table is written by
 * gen/main.go from colornames.go. */
static const bdfi_svg_named_color bdfi_svg_named_colors[] = {
    /* clang-format off */
    /* BEGIN GENERATED: colors */
    {"aliceblue", 0xf0f8ff},
    {"antiquewhite", 0xfaebd7},
    {"aqua", 0x00ffff},
    {"aquamarine", 0x7fffd4},
    {"azure", 0xf0ffff},
    {"beige", 0xf5f5dc},
    {"bisque", 0xffe4c4},
    {"black", 0x000000},
    {"blanchedalmond", 0xffebcd},
    {"blue", 0x0000ff},
    {"blueviolet", 0x8a2be2},
    {"brown", 0xa52a2a},
    {"burlywood", 0xdeb887},
    {"cadetblue", 0x5f9ea0},
    {"chartreuse", 0x7fff00},
    {"chocolate", 0xd2691e},
    {"coral", 0xff7f50},
    {"cornflowerblue", 0x6495ed},
    {"cornsilk", 0xfff8dc},
    {"crimson", 0xdc143c},
    {"cyan", 0x00ffff},
    {"darkblue", 0x00008b},
    {"darkcyan", 0x008b8b},
    {"darkgoldenrod", 0xb8860b},
    {"darkgray", 0xa9a9a9},
    {"darkgreen", 0x006400},
    {"darkgrey", 0xa9a9a9},
    {"darkkhaki", 0xbdb76b},
    {"darkmagenta", 0x8b008b},
    {"darkolivegreen", 0x556b2f},
    {"darkorange", 0xff8c00},
    {"darkorchid", 0x9932cc},
    {"darkred", 0x8b0000},
    {"darksalmon", 0xe9967a},
    {"darkseagreen", 0x8fbc8f},
    {"darkslateblue", 0x483d8b},
    {"darkslategray", 0x2f4f4f},
    {"darkslategrey", 0x2f4f4f},
    {"darkturquoise", 0x00ced1},
    {"darkviolet", 0x9400d3},
    {"deeppink", 0xff1493},
    {"deepskyblue", 0x00bfff},
    {"dimgray", 0x696969},
    {"dimgrey", 0x696969},
    {"dodgerblue", 0x1e90ff},
    {"firebrick", 0xb22222},
    {"floralwhite", 0xfffaf0},
    {"forestgreen", 0x228b22},
    {"fuchsia", 0xff00ff},
    {"gainsboro", 0xdcdcdc},
    {"ghostwhite", 0xf8f8ff},
    {"gold", 0xffd700},
    {"goldenrod", 0xdaa520},
    {"gray", 0x808080},
    {"green", 0x008000},
    {"greenyellow", 0xadff2f},
    {"grey", 0x808080},
    {"honeydew", 0xf0fff0},
    {"hotpink", 0xff69b4},
    {"indianred", 0xcd5c5c},
    {"indigo", 0x4b0082},
    {"ivory", 0xfffff0},
    {"khaki", 0xf0e68c},
    {"lavender", 0xe6e6fa},
    {"lavenderblush", 0xfff0f5},
    {"lawngreen", 0x7cfc00},
    {"lemonchiffon", 0xfffacd},
    {"lightblue", 0xadd8e6},
    {"lightcoral", 0xf08080},
    {"lightcyan", 0xe0ffff},
    {"lightgoldenrodyellow", 0xfafad2},
    {"lightgray", 0xd3d3d3},
    {"lightgreen", 0x90ee90},
    {"lightgrey", 0xd3d3d3},
    {"lightpink", 0xffb6c1},
    {"lightsalmon", 0xffa07a},
    {"lightseagreen", 0x20b2aa},
    {"lightskyblue", 0x87cefa},
    {"lightslategray", 0x778899},
    {"lightslategrey", 0x778899},
    {"lightsteelblue", 0xb0c4de},
    {"lightyellow", 0xffffe0},
    {"lime", 0x00ff00},
    {"limegreen", 0x32cd32},
    {"linen", 0xfaf0e6},
    {"magenta", 0xff00ff},
    {"maroon", 0x800000},
    {"mediumaquamarine", 0x66cdaa},
    {"mediumblue", 0x0000cd},
    {"mediumorchid", 0xba55d3},
    {"mediumpurple", 0x9370db},
    {"mediumseagreen", 0x3cb371},
    {"mediumslateblue", 0x7b68ee},
    {"mediumspringgreen", 0x00fa9a},
    {"mediumturquoise", 0x48d1cc},
    {"mediumvioletred", 0xc71585},
    {"midnightblue", 0x191970},
    {"mintcream", 0xf5fffa},
    {"mistyrose", 0xffe4e1},
    {"moccasin", 0xffe4b5},
    {"navajowhite", 0xffdead},
    {"navy", 0x000080},
    {"oldlace", 0xfdf5e6},
    {"olive", 0x808000},
    {"olivedrab", 0x6b8e23},
    {"orange", 0xffa500},
    {"orangered", 0xff4500},
    {"orchid", 0xda70d6},
    {"palegoldenrod", 0xeee8aa},
    {"palegreen", 0x98fb98},
    {"paleturquoise", 0xafeeee},
    {"palevioletred", 0xdb7093},
    {"papayawhip", 0xffefd5},
    {"peachpuff", 0xffdab9},
    {"peru", 0xcd853f},
    {"pink", 0xffc0cb},
    {"plum", 0xdda0dd},
    {"powderblue", 0xb0e0e6},
    {"purple", 0x800080},
    {"rebeccapurple", 0x663399},
    {"red", 0xff0000},
    {"rosybrown", 0xbc8f8f},
    {"royalblue", 0x4169e1},
    {"saddlebrown", 0x8b4513},
    {"salmon", 0xfa8072},
    {"sandybrown", 0xf4a460},
    {"seagreen", 0x2e8b57},
    {"seashell", 0xfff5ee},
    {"sienna", 0xa0522d},
    {"silver", 0xc0c0c0},
    {"skyblue", 0x87ceeb},
    {"slateblue", 0x6a5acd},
    {"slategray", 0x708090},
    {"slategrey", 0x708090},
    {"snow", 0xfffafa},
    {"springgreen", 0x00ff7f},
    {"steelblue", 0x4682b4},
    {"tan", 0xd2b48c},
    {"teal", 0x008080},
    {"thistle", 0xd8bfd8},
    {"tomato", 0xff6347},
    {"turquoise", 0x40e0d0},
    {"violet", 0xee82ee},
    {"wheat", 0xf5deb3},
    {"white", 0xffffff},
    {"whitesmoke", 0xf5f5f5},
    {"yellow", 0xffff00},
    {"yellowgreen", 0x9acd32},
    /* END GENERATED: colors */
    /* clang-format on */
};

static int bdfi_svg_hex_digit(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

/* strconv.ParseFloat(strings.ToLower(p), 64) with its error dropped: 0 for
 * what is no number, an infinity for one out of range. */
static double bdfi_svg_color_float(bdfi_svg_str p) {
  double v;
  int ascii = 1;
  for (size_t i = 0; i < p.n; i++) {
    if ((uint8_t)p.p[i] >= 0x80) {
      ascii = 0;
      break;
    }
  }
  if (ascii) {
    /* ParseFloat reads capitals as small letters */
    (void)bdfi_parse_float(p.p, p.n, &v);
    return v;
  }
  /* With a character that is not ASCII it is a number only when that
   * character has a small letter i: an infinity, in at most 9 letters. */
  char buf[16];
  size_t k = bdfi_svg_lower_ascii(p, buf, sizeof buf);
  if (k == SIZE_MAX) {
    return 0;
  }
  (void)bdfi_parse_float(buf, k, &v);
  return v;
}

/* num in ParseColor: a part of rgb() or hsl(), as a share of scale. */
static double bdfi_svg_color_num(bdfi_svg_str p, double scale) {
  if (p.n > 0 && p.p[p.n - 1] == '%') {
    p.n--;
    return bdfi_svg_color_float(p) / 100;
  }
  if (p.n >= 3 && (p.p[p.n - 3] == 'd' || p.p[p.n - 3] == 'D') &&
      (p.p[p.n - 2] == 'e' || p.p[p.n - 2] == 'E') &&
      (p.p[p.n - 1] == 'g' || p.p[p.n - 1] == 'G')) {
    p.n -= 3;
  }
  return bdfi_svg_color_float(p) / scale;
}

static double bdfi_svg_clamp(double v) {
  return bdfi_fmin(1, bdfi_fmax(0, v));
}

/* hue in hslToRGB. */
static double bdfi_svg_hue(double p, double q, double t) {
  if (t < 0) {
    t++;
  }
  if (t > 1) {
    t--;
  }
  if (t < 0.16666666666666666) { /* 1.0/6 */
    return p + (q - p) * 6 * t;
  }
  if (t < 0.5) {
    return q;
  }
  if (t < 0.66666666666666663) { /* 2.0/3 */
    return p + (q - p) * (0.66666666666666663 - t) * 6;
  }
  return p;
}

/* hslToRGB in color.go. */
static void bdfi_svg_hsl(double h, double s, double l, bdfi_svg_color *out) {
  if (s == 0) {
    out->r = out->g = out->b = l;
    return;
  }
  double q = l * (1 + s);
  if (l >= 0.5) {
    q = l + s - l * s;
  }
  double p = 2 * l - q;
  out->r = bdfi_svg_hue(p, q, h + 0.33333333333333331); /* 1.0/3 */
  out->g = bdfi_svg_hue(p, q, h);
  out->b = bdfi_svg_hue(p, q, h - 0.33333333333333331);
}

/* ParseColor in color.go. */
int bdfi_svg_parse_color(const char *s, size_t n, bdfi_svg_color current, bdfi_svg_color *out) {
  out->r = out->g = out->b = out->a = 0;
  /* s = strings.TrimSpace(strings.ToLower(s)): the white space is the same
   * before and after the letters are made small */
  bdfi_svg_str t = bdfi_svg_trim_space(bdfi_svg_str_of(s, n));
  char word[21]; /* the longest name has 20 letters */
  size_t wl = bdfi_svg_lower_ascii(t, word, sizeof word - 1);
  if (wl == 12 && memcmp(word, "currentcolor", 12) == 0) {
    *out = current;
    return 1;
  }
  if (wl == 11 && memcmp(word, "transparent", 11) == 0) {
    return 1;
  }
  if (t.n > 0 && t.p[0] == '#') {
    /* strconv.ParseUint(h, 16, 64) fails for what is not hexadecimal digits */
    int d[8];
    size_t hl = t.n - 1;
    if (hl != 3 && hl != 4 && hl != 6 && hl != 8) {
      return 0;
    }
    for (size_t i = 0; i < hl; i++) {
      d[i] = bdfi_svg_hex_digit(t.p[1 + i]);
      if (d[i] < 0) {
        return 0;
      }
    }
    if (hl <= 4) {
      out->r = (double)(d[0] * 17) / 255;
      out->g = (double)(d[1] * 17) / 255;
      out->b = (double)(d[2] * 17) / 255;
      out->a = hl == 4 ? (double)(d[3] * 17) / 255 : 1;
    } else {
      out->r = (double)(d[0] * 16 + d[1]) / 255;
      out->g = (double)(d[2] * 16 + d[3]) / 255;
      out->b = (double)(d[4] * 16 + d[5]) / 255;
      out->a = hl == 8 ? (double)(d[6] * 16 + d[7]) / 255 : 1;
    }
    return 1;
  }
  int rgb = t.n >= 3 && (t.p[0] == 'r' || t.p[0] == 'R') && (t.p[1] == 'g' || t.p[1] == 'G') &&
            (t.p[2] == 'b' || t.p[2] == 'B');
  int hsl = t.n >= 3 && (t.p[0] == 'h' || t.p[0] == 'H') && (t.p[1] == 's' || t.p[1] == 'S') &&
            (t.p[2] == 'l' || t.p[2] == 'L');
  if (rgb || hsl) {
    const char *open = memchr(t.p, '(', t.n);
    const char *close = NULL;
    for (size_t i = t.n; i > 0; i--) { /* strings.LastIndexByte */
      if (t.p[i - 1] == ')') {
        close = t.p + i - 1;
        break;
      }
    }
    if (!open || !close || close < open) {
      return 0;
    }
    /* strings.FieldsFunc: the parts between commas, spaces and slashes */
    bdfi_svg_str parts[4];
    size_t np = 0;
    const char *p = open + 1;
    while (p < close && np < 4) {
      while (p < close && (*p == ',' || *p == ' ' || *p == '/')) {
        p++;
      }
      const char *start = p;
      while (p < close && *p != ',' && *p != ' ' && *p != '/') {
        p++;
      }
      if (p > start) {
        parts[np++] = bdfi_svg_str_of(start, (size_t)(p - start));
      }
    }
    if (np < 3) {
      return 0;
    }
    double a = 1.0;
    if (np >= 4) {
      a = bdfi_svg_color_num(parts[3], 1);
    }
    if (rgb) {
      out->r = bdfi_svg_clamp(bdfi_svg_color_num(parts[0], 255));
      out->g = bdfi_svg_clamp(bdfi_svg_color_num(parts[1], 255));
      out->b = bdfi_svg_clamp(bdfi_svg_color_num(parts[2], 255));
      out->a = bdfi_svg_clamp(a);
      return 1;
    }
    double hh = bdfi_mod(bdfi_svg_color_num(parts[0], 1), 360) / 360;
    if (hh < 0) {
      hh++;
    }
    double sat = bdfi_svg_clamp(bdfi_svg_color_num(parts[1], 100));
    double l = bdfi_svg_clamp(bdfi_svg_color_num(parts[2], 100));
    bdfi_svg_hsl(hh, sat, l, out);
    out->a = bdfi_svg_clamp(a);
    return 1;
  }
  if (wl == SIZE_MAX) {
    return 0;
  }
  size_t lo = 0, hi = sizeof bdfi_svg_named_colors / sizeof bdfi_svg_named_colors[0];
  while (lo < hi) {
    size_t m = lo + (hi - lo) / 2;
    const bdfi_svg_named_color *c = &bdfi_svg_named_colors[m];
    size_t cl = strlen(c->name);
    int cmp = memcmp(word, c->name, wl < cl ? wl : cl);
    if (cmp == 0 && wl == cl) {
      out->r = (double)(c->rgb >> 16) / 255;
      out->g = (double)((c->rgb >> 8) & 0xff) / 255;
      out->b = (double)(c->rgb & 0xff) / 255;
      out->a = 1;
      return 1;
    }
    if (cmp < 0 || (cmp == 0 && wl < cl)) {
      hi = m;
    } else {
      lo = m + 1;
    }
  }
  return 0;
}
