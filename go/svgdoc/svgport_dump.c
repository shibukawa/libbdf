/* The C reader's document as text, for the tests: not a part of the
 * library. dump.go writes the reference's document the same way. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "svg_internal.h"
#include "svgport.h"

typedef struct svgport_out {
  char *p;
  size_t n, cap;
  int oom;
} svgport_out;

static void svgport_put(svgport_out *o, const char *s, size_t n) {
  if (o->oom) {
    return;
  }
  if (o->n + n + 1 > o->cap) {
    size_t cap = o->cap ? o->cap * 2 : 4096;
    while (cap < o->n + n + 1) {
      cap *= 2;
    }
    char *p = realloc(o->p, cap);
    if (!p) {
      o->oom = 1;
      return;
    }
    o->p = p;
    o->cap = cap;
  }
  memcpy(o->p + o->n, s, n);
  o->n += n;
}

static void svgport_puts(svgport_out *o, const char *s) {
  svgport_put(o, s, strlen(s));
}

/* A string with everything but printable ASCII escaped. */
static void svgport_quoted(svgport_out *o, bdfi_svg_str s) {
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < s.n; i++) {
    unsigned char c = (unsigned char)s.p[i];
    if (c >= 0x20 && c < 0x7f && c != '\\') {
      svgport_put(o, (const char *)&c, 1);
    } else {
      char e[4] = {'\\', 'x', hex[c >> 4], hex[c & 15]};
      svgport_put(o, e, 4);
    }
  }
}

static void svgport_int(svgport_out *o, long long v) {
  char b[32];
  snprintf(b, sizeof b, "%lld", v);
  svgport_puts(o, b);
}

static void svgport_float(svgport_out *o, double v) {
  if (v != v) {
    svgport_puts(o, "nan");
    return;
  }
  uint64_t u;
  char b[32];
  memcpy(&u, &v, sizeof u);
  snprintf(b, sizeof b, "%016llx", (unsigned long long)u);
  svgport_puts(o, b);
}

/* A float to 24 binary places where the reference's arithmetic is fused. */
static void svgport_near(svgport_out *o, double v, int exact) {
  if (exact || !(v >= -4 && v <= 4)) {
    svgport_float(o, v);
    return;
  }
  svgport_int(o, (long long)floor(v * 16777216.0 + 0.5));
}

static void svgport_values_of(svgport_out *o, bdfi_svg_str v, int exact) {
  double f;
  size_t rest;
  int ok = bdfi_svg_parse_number(v.p, v.n, &f, &rest);
  svgport_puts(o, "n\t");
  svgport_int(o, ok);
  svgport_puts(o, "\t");
  svgport_float(o, f);
  svgport_puts(o, "\t");
  svgport_int(o, (long long)(v.n - rest));
  svgport_puts(o, "\nN\t");
  size_t count = bdfi_svg_parse_numbers(v.p, v.n, NULL, 0);
  double *nums = malloc((count ? count : 1) * sizeof *nums);
  if (!nums) {
    o->oom = 1;
    return;
  }
  if (bdfi_svg_parse_numbers(v.p, v.n, nums, count) != count) {
    abort();
  }
  svgport_int(o, (long long)count);
  for (size_t i = 0; i < count; i++) {
    svgport_puts(o, "\t");
    svgport_float(o, nums[i]);
  }
  free(nums);
  ok = bdfi_svg_parse_length(v.p, v.n, 100, 16, &f);
  svgport_puts(o, "\nL\t");
  svgport_int(o, ok);
  svgport_puts(o, "\t");
  svgport_float(o, f);
  bdfi_svg_color cur = {0.25, 0.5, 0.75, 1}, c;
  ok = bdfi_svg_parse_color(v.p, v.n, cur, &c);
  svgport_puts(o, "\nC\t");
  svgport_int(o, ok);
  svgport_puts(o, "\t");
  svgport_near(o, c.r, exact);
  svgport_puts(o, "\t");
  svgport_near(o, c.g, exact);
  svgport_puts(o, "\t");
  svgport_near(o, c.b, exact);
  svgport_puts(o, "\t");
  svgport_near(o, c.a, exact);
  svgport_puts(o, "\n");
  if (exact) {
    bdfi_mat m = bdfi_svg_parse_transform(v.p, v.n);
    svgport_puts(o, "M");
    for (int i = 0; i < 6; i++) {
      svgport_puts(o, "\t");
      svgport_float(o, m.v[i]);
    }
    svgport_puts(o, "\n");
  }
}

static void svgport_declared(svgport_out *o, const bdfi_svg_node *n, bdfi_svg_str name) {
  bdfi_svg_str v;
  int ok = bdfi_svg_declared(n, name.p, name.n, &v);
  svgport_puts(o, "d\t");
  svgport_quoted(o, name);
  svgport_puts(o, ok ? "\t1\t" : "\t0\t");
  svgport_quoted(o, v);
  svgport_puts(o, "\n");
}

/* The properties and attributes the renderer asks for (raster/imagebdf/svg.go). */
static const char *const svgport_names[] = {"color",
                                            "fill",
                                            "stroke",
                                            "display",
                                            "opacity",
                                            "overflow",
                                            "clip-path",
                                            "mask",
                                            "stop-color",
                                            "stop-opacity",
                                            "font-family",
                                            "font-size",
                                            "font-style",
                                            "font-weight",
                                            "letter-spacing",
                                            "stroke-dasharray",
                                            "stroke-dashoffset",
                                            "stroke-linecap",
                                            "stroke-linejoin",
                                            "stroke-miterlimit",
                                            "stroke-width",
                                            "text-anchor",
                                            "visibility",
                                            "transform",
                                            "style",
                                            "class",
                                            "id",
                                            "href",
                                            "x",
                                            "d"};

static void svgport_node(svgport_out *o, const bdfi_svg_node *n, long long depth, int deep,
                         int exact) {
  bdfi_svg_str v;
  /* a run of text is what is named so (as the reference tells one): an
   * element that a document names "#text" is one with attributes */
  if (n->is_text != bdfi_svg_str_eq(n->name, "#text", 5) || (!n->is_text && n->text.n) ||
      (n->is_text && (n->has_sheet || n->nstyle > n->nattrs))) {
    abort();
  }
  svgport_puts(o, n->is_text ? "T\t" : "E\t");
  svgport_int(o, depth);
  svgport_puts(o, "\t");
  svgport_quoted(o, n->name);
  svgport_puts(o, "\t");
  svgport_int(o, (long long)n->nchildren);
  svgport_puts(o, "\t");
  svgport_quoted(o, n->text);
  svgport_puts(o, "\n");
  for (size_t i = 0; i < n->nattrs; i++) {
    svgport_puts(o, "A\t");
    svgport_quoted(o, n->attrs[i].name);
    svgport_puts(o, "\t");
    svgport_quoted(o, n->attrs[i].value);
    svgport_puts(o, "\n");
    /* the look-ups have to find what the lists hold */
    if (!bdfi_svg_attr(n, n->attrs[i].name.p, n->attrs[i].name.n, &v) ||
        v.p != n->attrs[i].value.p) {
      abort();
    }
    if (deep) {
      svgport_values_of(o, n->attrs[i].value, exact);
    }
  }
  if (!n->has_sheet) {
    if (n->nsheet) {
      abort();
    }
    svgport_puts(o, "S\t-\n");
  } else {
    svgport_puts(o, "S\t");
    svgport_int(o, (long long)n->nsheet);
    svgport_puts(o, "\n");
    for (size_t i = 0; i < n->nsheet; i++) {
      svgport_puts(o, "D\t");
      svgport_quoted(o, n->sheet[i]->name);
      svgport_puts(o, "\t");
      svgport_quoted(o, n->sheet[i]->value);
      svgport_puts(o, "\n");
      if (!bdfi_svg_sheet(n, n->sheet[i]->name.p, n->sheet[i]->name.n, &v) ||
          v.p != n->sheet[i]->value.p) {
        abort();
      }
    }
  }
  if (deep && (!n->is_text || n->nattrs)) {
    for (size_t i = 0; i < n->nattrs; i++) {
      svgport_declared(o, n, n->attrs[i].name);
    }
    for (size_t i = 0; i < n->nsheet; i++) {
      svgport_declared(o, n, n->sheet[i]->name);
    }
    for (size_t i = 0; i < n->nstyle; i++) {
      svgport_declared(o, n, n->style[i].name);
    }
    for (size_t i = 0; i < sizeof svgport_names / sizeof svgport_names[0]; i++) {
      svgport_declared(o, n, bdfi_svg_str_of(svgport_names[i], strlen(svgport_names[i])));
    }
  }
}

/* A tree in the order of the document, without recursion. */
static void svgport_tree(svgport_out *o, const bdfi_svg_node *top, int deep, int exact) {
  const bdfi_svg_node *n = top;
  long long depth = 1;
  uint32_t seen = top->index;
  while (n) {
    /* the numbers of the nodes of a tree follow its top's in this order */
    if (n->index != seen++ || (n != top && !n->parent)) {
      abort();
    }
    svgport_node(o, n, depth, deep, exact);
    if (n->first_child) {
      if (n->first_child->parent != n) {
        abort();
      }
      n = n->first_child;
      depth++;
      continue;
    }
    while (n != top && !n->next) {
      if (n->parent->last_child != n) {
        abort();
      }
      n = n->parent;
      depth--;
    }
    if (n == top) {
      break;
    }
    if (n->next->parent != n->parent) {
      abort();
    }
    n = n->next;
  }
}

char *svgport_dump(const uint8_t *data, size_t len, int64_t max_depth, int64_t max_nodes,
                   int64_t max_matches, int deep, int exact, size_t *out_len) {
  bdfi_svg_options opts = {max_depth, max_nodes, max_matches};
  bdfi_svg_doc *doc = (bdfi_svg_doc *)(uintptr_t)1;
  bdf_error err;
  svgport_out o = {NULL, 0, 0, 0};
  bdf_status st = bdfi_svg_parse(data, len, &opts, &doc, &err);
  if (st != err.status || (st == BDF_OK) != (doc != NULL)) {
    abort();
  }
  if (st != BDF_OK) {
    const char *what = "?";
    if (st == BDF_ERR_NOMEM) {
      what = "nomem";
    } else if (err.message == bdfi_svg_msg_not_svg()) {
      what = "notsvg";
    } else if (err.message == bdfi_svg_msg_encoding()) {
      what = "encoding";
    } else if (err.message == bdfi_svg_msg_sheets()) {
      what = "sheets";
    } else if (err.message == bdfi_svg_msg_input()) {
      what = "input";
    }
    if ((st == BDF_ERR_FORMAT) !=
            (!strcmp(what, "notsvg") || !strcmp(what, "encoding")) ||
        (st == BDF_ERR_TOO_LARGE) !=
            (!strcmp(what, "sheets") || !strcmp(what, "input"))) {
      abort();
    }
    svgport_puts(&o, what);
    svgport_puts(&o, "\n");
  } else {
    svgport_puts(&o, "doc\n");
    if (!doc->root || doc->root->parent || !bdfi_svg_str_eq(doc->root->name, "svg", 3)) {
      abort();
    }
    svgport_tree(&o, doc->root, deep, exact);
    /* the elements by id: their tree (0 the root's, then those after it in
     * the order their ids come in) and their place in it */
    const bdfi_svg_node **tops = malloc((doc->nids + 1) * sizeof *tops);
    size_t ntops = 0;
    if (!tops) {
      o.oom = 1;
    }
    for (size_t i = 0; i < doc->nids && !o.oom; i++) {
      const bdfi_svg_node *n = doc->ids[i].node, *top = n;
      bdfi_svg_str v;
      if (i > 0 && bdfi_svg_str_cmp(doc->ids[i - 1].id, doc->ids[i].id) >= 0) {
        abort();
      }
      if (bdfi_svg_by_id(doc, doc->ids[i].id.p, doc->ids[i].id.n) != n ||
          !bdfi_svg_attr(n, "id", 2, &v) || bdfi_svg_str_cmp(v, doc->ids[i].id) != 0 || v.n == 0) {
        abort();
      }
      while (top->parent) {
        top = top->parent;
      }
      size_t t = 0;
      if (top != doc->root) {
        for (t = 0; t < ntops && tops[t] != top; t++) {
        }
        if (t == ntops) {
          tops[ntops++] = top;
        }
        t++;
      }
      svgport_puts(&o, "I\t");
      svgport_quoted(&o, doc->ids[i].id);
      svgport_puts(&o, "\t");
      svgport_int(&o, (long long)t);
      svgport_puts(&o, "\t");
      if (n->index >= doc->nnodes || n->index < top->index) {
        abort();
      }
      svgport_int(&o, (long long)(n->index - top->index));
      svgport_puts(&o, "\n");
    }
    for (size_t t = 0; t < ntops; t++) {
      svgport_puts(&o, "O\t");
      svgport_int(&o, (long long)t + 1);
      svgport_puts(&o, "\n");
      svgport_tree(&o, tops[t], deep, exact);
    }
    free(tops);
    for (size_t i = 0; i < doc->nwarnings; i++) {
      svgport_puts(&o, "W\t");
      svgport_puts(&o, doc->warnings[i]);
      svgport_puts(&o, "\n");
    }
    bdfi_svg_free(doc);
  }
  if (o.oom) {
    free(o.p);
    return NULL;
  }
  *out_len = o.n;
  return o.p;
}

void svgport_floats(const char *s, const uint32_t *ends, size_t count, uint64_t *bits,
                    int8_t *status) {
  uint32_t start = 0;
  for (size_t i = 0; i < count; i++) {
    double v;
    status[i] = (int8_t)bdfi_parse_float(s + start, ends[i] - start, &v);
    memcpy(&bits[i], &v, sizeof v);
    start = ends[i];
  }
}

char *svgport_values(const char *s, size_t n, int exact, size_t *out_len) {
  svgport_out o = {NULL, 0, 0, 0};
  svgport_values_of(&o, bdfi_svg_str_of(s, n), exact);
  if (o.oom) {
    free(o.p);
    return NULL;
  }
  *out_len = o.n;
  return o.p;
}

void svgport_runes(uint32_t from, uint32_t to, uint32_t *lower, uint8_t *space) {
  for (uint32_t r = from; r < to; r++) {
    lower[r - from] = bdfi_svg_to_lower(r);
    space[r - from] = (uint8_t)bdfi_svg_is_space(r);
  }
}

char *svgport_strings(const char *s, size_t n, size_t *out_len) {
  svgport_out o = {NULL, 0, 0, 0};
  bdfi_svg_str str = bdfi_svg_str_of(s, n), f;
  svgport_puts(&o, "trim\t");
  svgport_quoted(&o, bdfi_svg_trim_space(str));
  svgport_puts(&o, "\nlower\t");
  size_t ln = bdfi_svg_lower(s, n, NULL);
  char *low = malloc(ln + 1);
  if (!low) {
    free(o.p);
    return NULL;
  }
  if (bdfi_svg_lower(s, n, low) != ln) {
    abort();
  }
  svgport_quoted(&o, bdfi_svg_str_of(low, ln));
  free(low);
  svgport_puts(&o, "\nfields");
  size_t pos = 0;
  while (bdfi_svg_next_field(str, &pos, &f)) {
    svgport_puts(&o, "\t");
    svgport_quoted(&o, f);
  }
  svgport_puts(&o, "\n");
  if (o.oom) {
    free(o.p);
    return NULL;
  }
  *out_len = o.n;
  return o.p;
}

void svgport_mat_mul(const double *m, const double *n, double *out) {
  bdfi_mat a, b;
  memcpy(a.v, m, sizeof a.v);
  memcpy(b.v, n, sizeof b.v);
  a = bdfi_svg_mat_mul(a, b);
  memcpy(out, a.v, sizeof a.v);
}

int svgport_read(const uint8_t *data, size_t len, int times) {
  bdf_status st = BDF_OK;
  for (int i = 0; i < times; i++) {
    bdfi_svg_doc *doc = NULL;
    st = bdfi_svg_parse(data, len, NULL, &doc, NULL);
    bdfi_svg_free(doc);
  }
  return (int)st;
}

size_t svgport_numbers(const char *s, size_t n, int times) {
  size_t count = 0;
  for (int i = 0; i < times; i++) {
    count = bdfi_svg_parse_numbers(s, n, NULL, 0);
  }
  return count;
}
