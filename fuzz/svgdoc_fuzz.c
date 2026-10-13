/* A libFuzzer target over the reader of SVG documents: whatever the bytes
 * are, it reads them as a document, with the limits of the reference and
 * with small ones, walks the whole tree, and asks every value function about
 * every attribute. It also asks them about the bytes themselves. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "svg_internal.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* Something the results are added into, so that they are all read. */
typedef struct bdfi_svg_fuzz_sink {
  double f;
  size_t n;
} bdfi_svg_fuzz_sink;

static void bdfi_svg_fuzz_bytes(bdfi_svg_fuzz_sink *sink, bdfi_svg_str s) {
  for (size_t i = 0; i < s.n; i++) {
    sink->n += (unsigned char)s.p[i];
  }
}

static void bdfi_svg_fuzz_value(bdfi_svg_fuzz_sink *sink, bdfi_svg_str v) {
  double f, few[4];
  size_t rest;
  bdfi_svg_color c, current = {0.25, 0.5, 0.75, 1};
  static const bdfi_mat identity = {{1, 0, 0, 1, 0, 0}};
  int ok = bdfi_svg_parse_number(v.p, v.n, &f, &rest);
  if (rest > v.n) {
    abort();
  }
  sink->f += ok ? f : 0;
  sink->n += bdfi_svg_parse_numbers(v.p, v.n, few, 4);
  if (bdfi_svg_parse_length(v.p, v.n, 100, 16, &f)) {
    sink->f += f;
  }
  if (bdfi_svg_parse_color(v.p, v.n, current, &c)) {
    sink->f += c.r + c.g + c.b + c.a;
  }
  bdfi_mat m = bdfi_svg_parse_transform(v.p, v.n);
  m = bdfi_svg_mat_mul(m, identity);
  sink->f += m.v[0] + m.v[5];
  if (bdfi_parse_float(v.p, v.n, &f) == 0) {
    sink->f += f;
  }
}

static void bdfi_svg_fuzz_node(bdfi_svg_fuzz_sink *sink, const bdfi_svg_node *n) {
  static const char *const names[6] = {"fill", "style", "transform", "id", "class", "href"};
  bdfi_svg_str v;
  bdfi_svg_fuzz_bytes(sink, n->name);
  bdfi_svg_fuzz_bytes(sink, n->text);
  /* a run of text, or an element a document names as they are */
  if (n->is_text != bdfi_svg_str_eq(n->name, "#text", 5) || (n->is_text && n->has_sheet) ||
      (!n->is_text && n->text.n)) {
    abort();
  }
  for (size_t i = 0; i < n->nattrs; i++) {
    bdfi_svg_fuzz_bytes(sink, n->attrs[i].name);
    bdfi_svg_fuzz_value(sink, n->attrs[i].value);
    if (!bdfi_svg_attr(n, n->attrs[i].name.p, n->attrs[i].name.n, &v) ||
        v.p != n->attrs[i].value.p ||
        !bdfi_svg_declared(n, n->attrs[i].name.p, n->attrs[i].name.n, &v)) {
      abort();
    }
    bdfi_svg_fuzz_bytes(sink, v);
  }
  for (size_t i = 0; i < n->nstyle; i++) {
    bdfi_svg_fuzz_bytes(sink, n->style[i].name);
    bdfi_svg_fuzz_value(sink, n->style[i].value);
    if (!bdfi_svg_declared(n, n->style[i].name.p, n->style[i].name.n, &v) ||
        v.p != n->style[i].value.p) {
      abort();
    }
  }
  if (n->nsheet && !n->has_sheet) {
    abort();
  }
  for (size_t i = 0; i < n->nsheet; i++) {
    bdfi_svg_fuzz_bytes(sink, n->sheet[i]->name);
    bdfi_svg_fuzz_value(sink, n->sheet[i]->value);
    if (!bdfi_svg_sheet(n, n->sheet[i]->name.p, n->sheet[i]->name.n, &v) ||
        v.p != n->sheet[i]->value.p ||
        !bdfi_svg_declared(n, n->sheet[i]->name.p, n->sheet[i]->name.n, &v)) {
      abort();
    }
  }
  for (size_t i = 0; i < 6; i++) {
    if (bdfi_svg_declared(n, names[i], strlen(names[i]), &v)) {
      bdfi_svg_fuzz_bytes(sink, v);
    }
  }
}

/* Walks a tree in the order of the document: its nodes have the numbers
 * that follow its top's, and no more of them or deeper ones than the limits
 * allow. */
static void bdfi_svg_fuzz_tree(bdfi_svg_fuzz_sink *sink, const bdfi_svg_doc *doc,
                               const bdfi_svg_node *top, int64_t max_depth) {
  const bdfi_svg_node *n = top;
  uint32_t index = top->index;
  int64_t depth = 1;
  while (n) {
    if (n->index != index++ || n->index >= doc->nnodes || depth > max_depth + 1 ||
        (depth > max_depth && (!n->is_text || n->nattrs || n->first_child))) {
      abort();
    }
    bdfi_svg_fuzz_node(sink, n);
    if (n->first_child) {
      if (n->first_child->parent != n) {
        abort();
      }
      n = n->first_child;
      depth++;
      continue;
    }
    while (n != top && !n->next) {
      n = n->parent;
      depth--;
    }
    if (n == top) {
      break;
    }
    n = n->next;
  }
}

static void bdfi_svg_fuzz_read(bdfi_svg_fuzz_sink *sink, const uint8_t *data, size_t size,
                               const bdfi_svg_options *opts) {
  bdfi_svg_doc *doc = NULL;
  bdf_error err;
  bdf_status st = bdfi_svg_parse(data, size, opts, &doc, &err);
  if (st != BDF_OK) {
    if (doc || err.status != st ||
        (err.message != bdfi_svg_msg_not_svg() && err.message != bdfi_svg_msg_encoding() &&
         err.message != bdfi_svg_msg_selector() && err.message != bdfi_svg_msg_entities() &&
         err.message != bdfi_svg_msg_sheets() && err.message != bdfi_svg_msg_input() &&
         st != BDF_ERR_NOMEM)) {
      abort();
    }
    return;
  }
  int64_t max_depth = opts ? opts->max_depth : BDFI_SVG_MAX_DEPTH;
  int64_t max_nodes = opts ? opts->max_nodes : BDFI_SVG_MAX_NODES;
  if (!doc || !doc->root || doc->root->parent || !bdfi_svg_str_eq(doc->root->name, "svg", 3) ||
      (int64_t)doc->nnodes > max_nodes) {
    abort();
  }
  bdfi_svg_fuzz_tree(sink, doc, doc->root, max_depth);
  for (size_t i = 0; i < doc->nids; i++) {
    const bdfi_svg_node *n = doc->ids[i].node, *top = n;
    bdfi_svg_str v;
    if (bdfi_svg_by_id(doc, doc->ids[i].id.p, doc->ids[i].id.n) != n ||
        !bdfi_svg_attr(n, "id", 2, &v) || bdfi_svg_str_cmp(v, doc->ids[i].id) != 0) {
      abort();
    }
    /* an element with an id may be in a tree after the root's */
    while (top->parent) {
      top = top->parent;
    }
    if (top != doc->root && top == n) {
      bdfi_svg_fuzz_tree(sink, doc, top, max_depth);
    }
  }
  for (size_t i = 0; i < doc->nwarnings; i++) {
    sink->n += strlen(doc->warnings[i]);
  }
  bdfi_svg_free(doc);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  /* the limits of the reference's own fuzz test: a small document reaches
   * them */
  static const bdfi_svg_options small = {40, 2000, 20000};
  bdfi_svg_fuzz_sink sink = {0, 0};
  bdfi_svg_fuzz_read(&sink, data, size, NULL);
  bdfi_svg_fuzz_read(&sink, data, size, &small);
  if (size <= 512) {
    bdfi_svg_fuzz_value(&sink, bdfi_svg_str_of(size ? (const char *)data : "", size));
  }
  return sink.n == SIZE_MAX && sink.f != sink.f;
}
