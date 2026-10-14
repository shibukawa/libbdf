/*
 * The tree of an SVG document: a port of svg.go of the reference's package
 * image/svg. The reader is lenient, as browsers are with the SVG images of
 * web pages: it reads the entities and the void elements of HTML, documents
 * in the encodings they declare, and a document with an error up to the
 * error. Only a document without an svg root element is refused.
 *
 * A document decides how much its reader keeps in memory, so reading has
 * limits: what passes one ends the reading there, and the warnings of the
 * document say so.
 */
#include <stdlib.h>
#include <string.h>

#include "svg_internal.h"

static const char bdfi_svg_not_svg[] = "svg: no svg root element";
static const char bdfi_svg_bad_encoding[] = "svg: the document is in an encoding that is not read";
static const char bdfi_svg_big_sheets[] =
    "svg: the style sheets declare more for the elements than is kept";
static const char bdfi_svg_big_input[] = "svg: the document is larger than is read";

const char *bdfi_svg_msg_not_svg(void) {
  return bdfi_svg_not_svg;
}
const char *bdfi_svg_msg_encoding(void) {
  return bdfi_svg_bad_encoding;
}
const char *bdfi_svg_msg_sheets(void) {
  return bdfi_svg_big_sheets;
}
const char *bdfi_svg_msg_input(void) {
  return bdfi_svg_big_input;
}

int bdfi_svg_attr(const bdfi_svg_node *n, const char *name, size_t len, bdfi_svg_str *out) {
  const bdfi_svg_decl *d = bdfi_svg_find(n->attrs, n->nattrs, name, len);
  *out = d ? d->value : bdfi_svg_str_of("", 0);
  return d != NULL;
}

int bdfi_svg_sheet(const bdfi_svg_node *n, const char *name, size_t len, bdfi_svg_str *out) {
  bdfi_svg_str want = bdfi_svg_str_of(name, len);
  size_t lo = 0, hi = n->nsheet;
  while (lo < hi) {
    size_t m = lo + (hi - lo) / 2;
    int c = bdfi_svg_str_cmp(n->sheet[m]->name, want);
    if (c == 0) {
      *out = n->sheet[m]->value;
      return 1;
    }
    if (c < 0) {
      lo = m + 1;
    } else {
      hi = m;
    }
  }
  *out = bdfi_svg_str_of("", 0);
  return 0;
}

const bdfi_svg_node *bdfi_svg_by_id(const bdfi_svg_doc *doc, const char *id, size_t len) {
  bdfi_svg_str want = bdfi_svg_str_of(id, len);
  size_t lo = 0, hi = doc->nids;
  while (lo < hi) {
    size_t m = lo + (hi - lo) / 2;
    int c = bdfi_svg_str_cmp(doc->ids[m].id, want);
    if (c == 0) {
      return doc->ids[m].node;
    }
    if (c < 0) {
      lo = m + 1;
    } else {
      hi = m;
    }
  }
  return NULL;
}

void bdfi_svg_free(bdfi_svg_doc *doc) {
  if (doc) {
    bdfi_arena_free(&doc->arena);
    free(doc);
  }
}

/* ---- Reading -------------------------------------------------------------- */

/* An attribute of a start tag as the reader gave it. */
typedef struct bdfi_svg_raw_attr {
  bdfi_svg_str name, value; /* the local name, and the value as written */
  int prefixed;
} bdfi_svg_raw_attr;

static int bdfi_svg_raw_attr_cmp(const void *a, const void *b) {
  return bdfi_svg_str_cmp(((const bdfi_svg_raw_attr *)a)->name,
                          ((const bdfi_svg_raw_attr *)b)->name);
}

static int bdfi_svg_id_cmp(const void *a, const void *b) {
  return bdfi_svg_str_cmp(((const bdfi_svg_id *)a)->id, ((const bdfi_svg_id *)b)->id);
}

/* What Parse keeps while it reads. */
typedef struct bdfi_svg_parser {
  bdfi_svg_doc *doc;
  bdfi_svg_pool pool;
  bdfi_xml_reader xml;
  bdfi_svg_node **stack; /* the open elements */
  size_t nstack, stack_cap;
  bdfi_svg_raw_attr *attrs; /* of the start tag being read */
  size_t attrs_cap;
  bdfi_svg_id *ids; /* every element with an id, in the order of the document */
  size_t nids, ids_cap;
  bdfi_xml_buf text;   /* a value or a run of text, decoded */
  bdfi_xml_buf styles; /* strings.Join(styles, "\n") */
  size_t nstyles;
  const char *warnings[2];
  size_t nwarnings;
} bdfi_svg_parser;

/* A string of the document, decoded as Value.String decodes it. */
static int bdfi_svg_decoded(bdfi_svg_parser *p, bdfi_svg_str raw, bdfi_svg_str *out) {
  if (raw.n == 0 || (!memchr(raw.p, '&', raw.n) && !memchr(raw.p, '\r', raw.n))) {
    return bdfi_svg_pool_dup(&p->pool, raw.p, raw.n, out);
  }
  p->text.n = 0;
  if (bdfi_xml_unescape(&p->text, raw) != 0) {
    return -1;
  }
  return bdfi_svg_pool_dup(&p->pool, (const char *)p->text.p, p->text.n, out);
}

/*
 * The attributes of the start tag the reader is on, as the loop of Parse
 * puts them into Node.Attr: by their local names, the last of a name; the
 * declarations of namespaces are left out, and a prefixed href (xlink:href)
 * does not replace an href that is there.
 */
static int bdfi_svg_read_attrs(bdfi_svg_parser *p, bdfi_svg_node *n) {
  bdfi_svg_str name, value;
  size_t count = 0;
  while (bdfi_xml_next_attr(&p->xml, &name, &value)) {
    bdfi_svg_str local = bdfi_xml_local(name);
    int prefixed = local.n < name.n;
    if (bdfi_svg_str_eq(local, "xmlns", 5) ||
        (prefixed && name.n - local.n == 6 && memcmp(name.p, "xmlns:", 6) == 0)) {
      continue;
    }
    if (count == p->attrs_cap) {
      size_t cap = p->attrs_cap ? p->attrs_cap * 2 : 16;
      bdfi_svg_raw_attr *a =
          cap > p->attrs_cap ? bdfi_realloc_array(p->attrs, cap, sizeof *a) : NULL;
      if (!a) {
        return -1;
      }
      p->attrs = a;
      p->attrs_cap = cap;
    }
    p->attrs[count].name = local;
    p->attrs[count].value = value;
    p->attrs[count].prefixed = prefixed;
    count++;
  }
  if (count == 0) {
    return 0;
  }
  /* the sort is stable: the attributes of a name stay in their order */
  if (bdfi_svg_sort(p->attrs, count, sizeof *p->attrs, bdfi_svg_raw_attr_cmp) != 0) {
    return -1;
  }
  size_t names = 0;
  for (size_t i = 0; i < count;) {
    size_t j = i + 1;
    while (j < count && bdfi_svg_str_cmp(p->attrs[i].name, p->attrs[j].name) == 0) {
      j++;
    }
    size_t winner = j - 1;
    if (bdfi_svg_str_eq(p->attrs[i].name, "href", 4)) {
      /* href wins over xlink:href, and the first xlink:href over the others */
      winner = i;
      for (size_t k = i + 1; k < j; k++) {
        if (!p->attrs[k].prefixed) {
          winner = k;
        }
      }
    }
    p->attrs[names++] = p->attrs[winner];
    i = j;
  }
  bdfi_svg_decl *attrs = bdfi_svg_pool_array(&p->pool, names, sizeof *attrs);
  if (!attrs) {
    return -1;
  }
  for (size_t i = 0; i < names; i++) {
    if (bdfi_svg_pool_dup(&p->pool, p->attrs[i].name.p, p->attrs[i].name.n, &attrs[i].name) != 0 ||
        bdfi_svg_decoded(p, p->attrs[i].value, &attrs[i].value) != 0) {
      return -1;
    }
  }
  n->attrs = attrs;
  n->nattrs = names;
  return 0;
}

/* fmt.Sprintf of a warning with one %d in it, into the arena. */
static const char *bdfi_svg_warning(bdfi_svg_parser *p, const char *before, int64_t v,
                                    const char *after) {
  char digits[20];
  size_t nd = 0;
  uint64_t u = (uint64_t)v; /* a limit is positive */
  do {
    digits[nd++] = (char)('0' + u % 10);
    u /= 10;
  } while (u != 0);
  size_t nb = strlen(before), na = strlen(after);
  char *s = bdfi_arena_alloc(&p->doc->arena, nb + nd + na + 1);
  if (!s) {
    return NULL;
  }
  memcpy(s, before, nb);
  for (size_t i = 0; i < nd; i++) {
    s[nb + i] = digits[nd - 1 - i];
  }
  memcpy(s + nb + nd, after, na + 1);
  return s;
}

static bdfi_svg_node *bdfi_svg_new_node(bdfi_svg_parser *p) {
  bdfi_svg_node *n = bdfi_arena_alloc(&p->doc->arena, sizeof *n);
  if (!n) {
    return NULL;
  }
  n->name = n->text = bdfi_svg_str_of("", 0);
  n->index = (uint32_t)p->doc->nnodes++;
  return n;
}

static void bdfi_svg_append_child(bdfi_svg_node *parent, bdfi_svg_node *n) {
  n->parent = parent;
  if (parent->last_child) {
    parent->last_child->next = n;
  } else {
    parent->first_child = n;
  }
  parent->last_child = n;
  parent->nchildren++;
}

/* The loop of Parse in svg.go. It returns BDF_OK when the reading ended, at
 * the end of the document, at an error or at a limit. */
static bdf_status bdfi_svg_read(bdfi_svg_parser *p, int64_t depth, int64_t most, const char **msg) {
  bdfi_svg_doc *doc = p->doc;
  int64_t nodes = 0;
  for (;;) {
    int k;
    int st = bdfi_xml_next(&p->xml, &k);
    if (st == BDFI_XML_NOMEM) {
      return BDF_ERR_NOMEM;
    }
    if (st == BDFI_XML_ENCODING) {
      *msg = bdfi_svg_msg_encoding();
      return BDF_ERR_FORMAT;
    }
    if (st != BDFI_XML_OK || k == BDFI_XML_EOF) {
      /* an error keeps what was read, as browsers draw up to an error */
      return BDF_OK;
    }
    if (k == BDFI_XML_START) {
      /* what passes a limit ends the reading as an error does */
      if ((int64_t)p->nstack >= depth) {
        p->warnings[p->nwarnings] =
            bdfi_svg_warning(p, "the elements of an SVG image are more than ", depth,
                             " deep: the rest are not read");
        return p->warnings[p->nwarnings++] ? BDF_OK : BDF_ERR_NOMEM;
      }
      nodes++;
      if (nodes > most) {
        p->warnings[p->nwarnings] = bdfi_svg_warning(p, "an SVG image has more than ", most,
                                                     " elements: the rest are not read");
        return p->warnings[p->nwarnings++] ? BDF_OK : BDF_ERR_NOMEM;
      }
      bdfi_svg_node *n = bdfi_svg_new_node(p);
      bdfi_svg_str local = bdfi_xml_local_name(&p->xml);
      if (!n || bdfi_svg_pool_dup(&p->pool, local.p, local.n, &n->name) != 0 ||
          bdfi_svg_read_attrs(p, n) != 0) {
        return BDF_ERR_NOMEM;
      }
      /* an element named as the runs of text are is one to the reference */
      n->is_text = bdfi_svg_str_eq(n->name, BDFI_SVG_TEXT_NODE, sizeof BDFI_SVG_TEXT_NODE - 1);
      bdfi_svg_str v;
      if (bdfi_svg_attr(n, "style", 5, &v) && v.n != 0 &&
          bdfi_svg_style_decls(&p->pool, v, &n->style, &n->nstyle) != 0) {
        return BDF_ERR_NOMEM;
      }
      if (p->nstack > 0) {
        bdfi_svg_append_child(p->stack[p->nstack - 1], n);
      } else if (!doc->root) {
        doc->root = n;
      }
      if (bdfi_svg_attr(n, "id", 2, &v) && v.n != 0) {
        if (p->nids == p->ids_cap) {
          size_t cap = p->ids_cap ? p->ids_cap * 2 : 16;
          bdfi_svg_id *ids = cap > p->ids_cap ? bdfi_realloc_array(p->ids, cap, sizeof *ids) : NULL;
          if (!ids) {
            return BDF_ERR_NOMEM;
          }
          p->ids = ids;
          p->ids_cap = cap;
        }
        p->ids[p->nids].id = v;
        p->ids[p->nids].node = n;
        p->nids++;
      }
      if (p->nstack == p->stack_cap) {
        size_t cap = p->stack_cap ? p->stack_cap * 2 : 32;
        bdfi_svg_node **s =
            cap > p->stack_cap ? bdfi_realloc_array(p->stack, cap, sizeof *s) : NULL;
        if (!s) {
          return BDF_ERR_NOMEM;
        }
        p->stack = s;
        p->stack_cap = cap;
      }
      p->stack[p->nstack++] = n;
    } else if (k == BDFI_XML_END) {
      if (p->nstack > 0) {
        p->nstack--;
      }
    } else if ((k == BDFI_XML_TEXT || k == BDFI_XML_CDATA) && p->nstack > 0) {
      bdfi_svg_node *parent = p->stack[p->nstack - 1];
      if (bdfi_svg_str_eq(parent->name, "style", 5)) {
        if ((p->nstyles > 0 && bdfi_xml_buf_append(&p->styles, "\n", 1) != 0) ||
            bdfi_xml_append_text(&p->styles, &p->xml) != 0) {
          return BDF_ERR_NOMEM;
        }
        p->nstyles++;
      } else if (bdfi_svg_str_eq(parent->name, "text", 4) ||
                 bdfi_svg_str_eq(parent->name, "tspan", 5) ||
                 bdfi_svg_str_eq(parent->name, "textPath", 8) ||
                 bdfi_svg_str_eq(parent->name, "a", 1)) {
        /* runs are kept where SVG draws them; one past the limit is left out
         * and the reading goes on */
        nodes++;
        if (nodes > most) {
          continue;
        }
        p->text.n = 0;
        if (bdfi_xml_append_text(&p->text, &p->xml) != 0) {
          return BDF_ERR_NOMEM;
        }
        bdfi_svg_node *n = bdfi_svg_new_node(p);
        if (!n || bdfi_svg_pool_dup(&p->pool, (const char *)p->text.p, p->text.n, &n->text) != 0) {
          return BDF_ERR_NOMEM;
        }
        n->name = bdfi_svg_str_of(BDFI_SVG_TEXT_NODE, sizeof BDFI_SVG_TEXT_NODE - 1);
        n->is_text = 1;
        bdfi_svg_append_child(parent, n);
      }
    }
  }
}

/* Document.IDs: the first of the elements that share an id. */
static int bdfi_svg_index_ids(bdfi_svg_parser *p) {
  /* the sort is stable: the elements of an id stay in their order */
  if (bdfi_svg_sort(p->ids, p->nids, sizeof *p->ids, bdfi_svg_id_cmp) != 0) {
    return -1;
  }
  size_t n = 0;
  for (size_t i = 0; i < p->nids; i++) {
    if (n == 0 || bdfi_svg_str_cmp(p->ids[n - 1].id, p->ids[i].id) != 0) {
      p->ids[n++] = p->ids[i];
    }
  }
  bdfi_svg_id *ids = bdfi_svg_pool_array(&p->pool, n, sizeof *ids);
  if (!ids) {
    return -1;
  }
  if (n > 0) {
    memcpy(ids, p->ids, n * sizeof *ids);
  }
  p->doc->ids = ids;
  p->doc->nids = n;
  return 0;
}

/* Parse in svg.go. */
bdf_status bdfi_svg_parse(const uint8_t *data, size_t len, const bdfi_svg_options *opts,
                          bdfi_svg_doc **out, bdf_error *err) {
  int64_t depth = BDFI_SVG_MAX_DEPTH, most = BDFI_SVG_MAX_NODES;
  int64_t matches = BDFI_SVG_MAX_STYLE_MATCHES;
  const char *msg = "";
  bdf_status st = BDF_OK;
  *out = NULL;
  if (opts) {
    if (opts->max_depth > 0) {
      depth = opts->max_depth;
    }
    if (opts->max_nodes > 0) {
      most = opts->max_nodes;
    }
    if (opts->max_style_matches > 0) {
      matches = opts->max_style_matches;
    }
  }
  bdfi_svg_parser p;
  memset(&p, 0, sizeof p);
  p.doc = len <= BDFI_SVG_MAX_INPUT ? bdfi_calloc_array(1, sizeof *p.doc) : NULL;
  if (!p.doc) {
    st = len <= BDFI_SVG_MAX_INPUT ? BDF_ERR_NOMEM : BDF_ERR_TOO_LARGE;
    msg = st == BDF_ERR_NOMEM ? "out of memory" : bdfi_svg_msg_input();
    bdfi_error_set(err, st, 0, msg);
    return st;
  }
  bdfi_arena_init(&p.doc->arena);
  p.pool.arena = &p.doc->arena;
  /* one level more than is read, so that the element that has it ends the
   * reading here; a limit that has no level more is the reader's default,
   * as an int that wraps makes it there */
  int64_t levels = depth == INT64_MAX ? 1024 : depth + 1;
  /* (elements are never more deep than the document has bytes) */
  if (bdfi_xml_open(&p.xml, data, len, levels > (int64_t)len ? len + 1 : (size_t)levels) !=
      BDFI_XML_OK) {
    st = BDF_ERR_NOMEM;
  }
  if (st == BDF_OK) {
    st = bdfi_svg_read(&p, depth, most, &msg);
  }
  if (st == BDF_OK && (!p.doc->root || !bdfi_svg_str_eq(p.doc->root->name, "svg", 3))) {
    st = BDF_ERR_FORMAT;
    msg = bdfi_svg_msg_not_svg();
  }
  if (st == BDF_OK && p.nstyles > 0) {
    int over = 0;
    st = bdfi_svg_apply_css(&p.pool,
                            bdfi_svg_str_of(p.styles.p ? (const char *)p.styles.p : "", p.styles.n),
                            p.doc->root, matches, &over, &msg);
    if (st == BDF_OK && over) {
      p.warnings[p.nwarnings++] =
          "the style sheets of an SVG image have too many rules for its elements: the rest of the "
          "elements are drawn without them";
    }
  }
  if (st == BDF_OK && bdfi_svg_index_ids(&p) != 0) {
    st = BDF_ERR_NOMEM;
  }
  if (st == BDF_OK && p.nwarnings > 0) {
    const char **w = bdfi_svg_pool_array(&p.pool, p.nwarnings, sizeof *w);
    if (w) {
      memcpy(w, p.warnings, p.nwarnings * sizeof *w);
      p.doc->warnings = w;
      p.doc->nwarnings = p.nwarnings;
    } else {
      st = BDF_ERR_NOMEM;
    }
  }
  bdfi_xml_close(&p.xml);
  bdfi_xml_buf_free(&p.text);
  bdfi_xml_buf_free(&p.styles);
  free(p.stack);
  free(p.attrs);
  free(p.ids);
  if (st != BDF_OK) {
    bdfi_svg_free(p.doc);
    bdfi_error_set(err, st, 0, st == BDF_ERR_NOMEM ? "out of memory" : msg);
    return st;
  }
  bdfi_error_clear(err);
  *out = p.doc;
  return BDF_OK;
}
