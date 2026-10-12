/* Tests of text extraction, links, the index parts and the text of views,
 * through the public API. The extraction cases are those of the reference's
 * bdf_test.go and malformed_test.go. */
#include "testutil.h"

/* ---- A resolver over objects by id -------------------------------------- */

typedef struct objs {
  bdf_object **o;
  size_t n;
} objs;

static const bdf_object *by_id(void *ctx, const uint8_t hash[16]) {
  const objs *t = ctx;
  size_t id = ((size_t)hash[0] << 24) | ((size_t)hash[1] << 16) | ((size_t)hash[2] << 8) | hash[3];
  return id < t->n ? t->o[id] : NULL;
}

static const bdf_object *none(void *ctx, const uint8_t hash[16]) {
  (void)ctx;
  (void)hash;
  return NULL;
}

/* expect_runs checks texts, separators and alt flags against "text:sep:alt"
 * entries joined by '|'. */
static void expect_runs(const bdf_text *t, const char *want, int line) {
  char got[1024] = "";
  size_t k = 0;
  for (size_t i = 0; i < bdf_text_run_count(t); i++) {
    bdf_text_run r;
    bdf_text_run_at(t, i, &r);
    k += (size_t)snprintf(got + k, sizeof got - k, "%s%s:%d:%d", i ? "|" : "", r.text, r.sep, r.alt_text);
    if (r.ordinal != i) {
      failures++;
      fprintf(stderr, "line %d: run %zu has ordinal %u\n", line, i, r.ordinal);
    }
  }
  if (strcmp(got, want) != 0) {
    failures++;
    fprintf(stderr, "line %d: runs\n  %s\nwant\n  %s\n", line, got, want);
  }
}

static void test_extract(void) {
  /* TestExtractTextAndIndex */
  ob c = {0};
  c.bbox[0] = 2;
  c.bbox[1] = 3;
  c.bbox[2] = 20;
  c.bbox[3] = 10;
  ob_set_font(&c, ob_font(&c, "serif"), 10);
  ob_text(&c, "child", 0, 0, 20);
  bdf_object *child = ob_decode(&c);

  ob o = {0};
  ob_set_font(&o, ob_font(&o, "sans-serif"), 12);
  ob_mark(&o, BDF_MARK_PARAGRAPH, "");
  ob_text(&o, "Hello", 0, 0, 30);
  ob_text(&o, "World", 35, 0, 30); /* a gap of 5 > 0.2 × size: a space */
  ob_mark(&o, BDF_MARK_LINE, "");
  ob_text(&o, "second", 0, 15, 40);
  ob_text(&o, "-line", 40, 15, 20); /* adjacent: nothing */
  ob_mark(&o, BDF_MARK_WRAP, "");
  ob_text(&o, "wrapped", 0, 30, 40); /* the next line, joined without a separator */
  size_t sq = ob_rect_path(&o);
  ob_mark(&o, BDF_MARK_ALT_TEXT, "outlined");
  ob_path_at(&o, sq, 5, 45); /* the ALT_TEXT describes the next drawing */
  size_t ref = ob_child(&o, 1);
  ob_op(&o, BDF_OP_SAVE);
  ob_translate(&o, 100, 100);
  ob_use(&o, ref);
  ob_op(&o, BDF_OP_RESTORE);
  ob_mark(&o, BDF_MARK_ALT_TEXT, "drawn by a child");
  ob_use_at(&o, ref, 50, 150); /* spans the child's bbox */
  bdf_object *top = ob_decode(&o);
  CHECK(top && child);

  bdf_object *table[] = {NULL, child};
  objs res = {table, 2};
  bdf_text *t = NULL;
  bdf_error err;
  CHECK(bdf_text_extract(top, by_id, &res, &t, &err) == BDF_OK);
  if (t) {
    expect_runs(t, "Hello:2:0|World:1:0|second:1:0|-line:0:0|wrapped:0:0|outlined:1:1|child:1:0|drawn by a child:1:1",
                __LINE__);
    bdf_text_run r;
    CHECK(bdf_text_run_at(t, 6, &r) == BDF_OK && r.x == 100 && r.y == 100 && r.has_font &&
          strcmp(r.font.family, "serif") == 0 && r.size == 10 && r.advance == 20);
    CHECK(r.matrix[4] == 100 && r.matrix[5] == 100 && r.matrix[0] == 1 && r.text_len == 5);
    CHECK(bdf_text_run_at(t, 5, &r) == BDF_OK && r.x == 5 && r.y == 45 && r.alt_text && r.advance == 0);
    CHECK(bdf_text_run_at(t, 7, &r) == BDF_OK && r.x == 52 && r.y == 150 && r.advance == 20 && r.alt_text);
    CHECK(bdf_text_run_at(t, 0, &r) == BDF_OK && r.has_font && strcmp(r.font.family, "sans-serif") == 0 &&
          r.size == 12 && r.align == BDF_ALIGN_LEFT);
    CHECK(bdf_text_run_at(t, 8, &r) == BDF_ERR_ARG);
    CHECK(bdf_text_link_count(t) == 0);
    bdf_text_free(t);
  }
  /* without the child: its run is gone, and the ALT_TEXT has no extent */
  CHECK(bdf_text_extract(top, none, NULL, &t, &err) == BDF_OK);
  if (t) {
    expect_runs(t, "Hello:2:0|World:1:0|second:1:0|-line:0:0|wrapped:0:0|outlined:1:1|drawn by a child:1:1", __LINE__);
    bdf_text_run r;
    CHECK(bdf_text_run_at(t, 6, &r) == BDF_OK && r.x == 50 && r.advance == 0);
    bdf_text_free(t);
  }
  CHECK(bdf_text_extract(top, NULL, NULL, &t, &err) == BDF_OK && bdf_text_run_count(t) == 7);
  bdf_text_free(t);
  CHECK(bdf_text_extract(NULL, NULL, NULL, &t, &err) == BDF_ERR_ARG);
  bdf_object_free(top);
  bdf_object_free(child);
}

static void test_structure_marks(void) {
  /* TestExtractTextStructureMarks: structure marks separate runs like
   * PARAGRAPH; LANG leaves the separator to the guess and keeps a pending
   * ALT_TEXT for its drawing */
  ob o = {0};
  size_t sq = ob_rect_path(&o);
  ob_set_font(&o, ob_font(&o, "sans-serif"), 10);
  ob_mark(&o, BDF_MARK_HEADING, "1");
  ob_text(&o, "Title", 0, 0, 25);
  ob_mark(&o, BDF_MARK_LIST, "");
  ob_mark(&o, BDF_MARK_LIST_ITEM, "");
  ob_text(&o, "one", 0, 15, 15);
  ob_mark(&o, BDF_MARK_LANG, "ja");
  ob_text(&o, "two", 17, 15, 15); /* the same line, a gap of 2 < 0.2 × size: nothing */
  ob_mark(&o, BDF_MARK_END, "");
  ob_mark(&o, BDF_MARK_TABLE, "");
  ob_mark(&o, BDF_MARK_CELL, "A1 col");
  ob_text(&o, "head", 0, 30, 20);
  ob_mark(&o, BDF_MARK_CELL, "A2");
  ob_text(&o, "cell", 0, 45, 20);
  ob_mark(&o, BDF_MARK_END, "");
  ob_mark(&o, BDF_MARK_ALT_TEXT, "icon");
  ob_mark(&o, BDF_MARK_LANG, "");
  ob_path_at(&o, sq, 0, 60);
  ob_mark(&o, BDF_MARK_FIGURE, "chart");
  ob_text(&o, "label", 0, 75, 25);
  ob_mark(&o, BDF_MARK_END, "");
  ob_mark(&o, BDF_MARK_ALT_TEXT, "left over"); /* nothing draws it: the last run, without a place */
  bdf_object *obj = ob_decode(&o);
  bdf_text *t = NULL;
  bdf_error err;
  CHECK(bdf_text_extract(obj, NULL, NULL, &t, &err) == BDF_OK);
  if (t) {
    expect_runs(t, "Title:2:0|one:2:0|two:0:0|head:2:0|cell:2:0|icon:2:1|label:2:0|left over:2:1", __LINE__);
    bdf_text_run r;
    CHECK(bdf_text_run_at(t, 5, &r) == BDF_OK && r.y == 60);
    CHECK(bdf_text_run_at(t, 7, &r) == BDF_OK && r.x == 0 && r.y == 0 && !r.has_font && r.size == 10);
    bdf_text_free(t);
  }
  bdf_object_free(obj);
}

/* ---- The structure -------------------------------------------------------- */

/* expect_nodes checks the nodes against entries joined by '|': the kind, the
 * parent, then what the node has of a level (l2), a cell (c0,1,2,2), a scope
 * (col), an alternative text (alt=…) and bounds (b10,20,30,40). */
static void expect_nodes(const bdf_text *t, const char *want, int line) {
  static const char *const kinds[] = {"par", "head", "cap", "list", "item", "table", "cell", "fig"};
  char got[4096] = "";
  size_t k = 0;
  for (size_t i = 0; i < bdf_text_node_count(t) && k < sizeof got - 256; i++) {
    bdf_text_node n;
    if (bdf_text_node_at(t, i, &n) != BDF_OK || n.kind > BDF_NODE_FIGURE) {
      failures++;
      fprintf(stderr, "line %d: node %zu\n", line, i);
      return;
    }
    k += (size_t)snprintf(got + k, sizeof got - k, "%s%s:%lld", i ? "|" : "", kinds[n.kind],
                          (long long)n.parent);
    if (n.level) {
      k += (size_t)snprintf(got + k, sizeof got - k, ":l%d", n.level);
    }
    if (n.has_cell) {
      k += (size_t)snprintf(got + k, sizeof got - k, ":c%u,%u,%u,%u", n.row, n.col, n.rows, n.cols);
    }
    if (n.scope) {
      k += (size_t)snprintf(got + k, sizeof got - k, ":%s", n.scope == BDF_SCOPE_COL ? "col" : "row");
    }
    if (n.kind == BDF_NODE_FIGURE) {
      k += (size_t)snprintf(got + k, sizeof got - k, ":alt=%s", n.alt);
    }
    if (n.has_bounds) {
      k += (size_t)snprintf(got + k, sizeof got - k, ":b%g,%g,%g,%g", (double)n.x, (double)n.y,
                            (double)n.w, (double)n.h);
    }
  }
  if (strcmp(got, want) != 0) {
    failures++;
    fprintf(stderr, "line %d: nodes\n  %s\nwant\n  %s\n", line, got, want);
  }
}

/* expect_run_nodes checks the node of each run against "0,1,1,2". */
static void expect_run_nodes(const bdf_text *t, const char *want, int line) {
  char got[1024] = "";
  size_t k = 0;
  for (size_t i = 0; i < bdf_text_run_count(t) && k < sizeof got - 32; i++) {
    bdf_text_run r;
    bdf_text_run_at(t, i, &r);
    k += (size_t)snprintf(got + k, sizeof got - k, "%s%lld", i ? "," : "", (long long)r.node);
  }
  if (strcmp(got, want) != 0) {
    failures++;
    fprintf(stderr, "line %d: the nodes of the runs\n  %s\nwant\n  %s\n", line, got, want);
  }
}

static bdf_text *extract(ob *o, objs *children) {
  bdf_object *obj = ob_decode(o);
  bdf_text *t = NULL;
  bdf_error err;
  if (!obj || bdf_text_extract(obj, children ? by_id : NULL, children, &t, &err) != BDF_OK) {
    failures++;
    fprintf(stderr, "extract: %s\n", obj ? err.message : "the object does not decode");
    exit(1);
  }
  /* the strings of the text point into the object, which is not freed: a test */
  return t;
}

static void test_nodes_leaves(void) {
  /* a leaf MARK chooses the kind of the leaf the next run opens; the last of
   * several decides; runs without one make a paragraph */
  ob o = {0};
  ob_text(&o, "first", 0, 0, 10);
  ob_mark(&o, BDF_MARK_HEADING, "2");
  ob_text(&o, "Title", 0, 20, 10);
  ob_mark(&o, BDF_MARK_PARAGRAPH, "");
  ob_text(&o, "Body", 0, 40, 10);
  ob_text(&o, "more", 10, 40, 10);
  ob_mark(&o, BDF_MARK_LINE, ""); /* a line of the same paragraph */
  ob_text(&o, "line", 0, 50, 10);
  ob_mark(&o, BDF_MARK_BOX, "");
  ob_mark(&o, BDF_MARK_HEADING, "x"); /* no level */
  ob_text(&o, "H", 0, 60, 10);
  ob_mark(&o, BDF_MARK_LIST_ITEM, ""); /* outside a list: a paragraph */
  ob_text(&o, "item", 0, 70, 10);
  ob_mark(&o, BDF_MARK_CELL, "B12"); /* outside a table: a cell of a sheet, a leaf */
  ob_text(&o, "cell", 0, 80, 10);
  ob_mark(&o, BDF_MARK_CELL, "C3:D5 row");
  ob_text(&o, "merged", 0, 90, 10);
  ob_mark(&o, BDF_MARK_END, ""); /* nothing is open: nothing ends, the leaf goes on */
  ob_text(&o, "tail", 0, 100, 10);
  bdf_text *t = extract(&o, NULL);
  expect_nodes(t, "par:-1|head:-1:l2|par:-1|head:-1|par:-1|cell:-1:c11,1,1,1|cell:-1:c2,2,3,2:row",
               __LINE__);
  expect_run_nodes(t, "0,1,2,2,2,3,4,5,6,6", __LINE__);
  bdf_text_node n;
  CHECK(bdf_text_node_at(t, 7, &n) == BDF_ERR_ARG);
  CHECK(bdf_text_node_at(NULL, 0, &n) == BDF_ERR_ARG);
  CHECK(bdf_text_node_at(t, 0, NULL) == BDF_ERR_ARG);
  CHECK(bdf_text_node_count(NULL) == 0);
  bdf_text_free(t);

  /* the levels JavaScript's Number() reads as 1 to 6, and what it does not */
  static const struct {
    const char *payload;
    int level;
  } levels[] = {
      {"1", 1},     {"6", 6},      {"7", 0},        {"0", 0},     {"", 0},      {" 3 ", 3},
      {"3.0", 3},   {"3.5", 0},    {"+4", 4},       {"-4", 0},    {"5e0", 5},   {"50e-1", 5},
      {".2e1", 2},  {"2.", 2},     {"0x6", 6},      {"0X6", 6},   {"0b101", 5}, {"0o7", 0},
      {"0o6", 6},   {"0x", 0},     {"1e", 0},       {"e1", 0},    {".", 0},     {"1 2", 0},
      {"three", 0}, {"0.00003e5", 3}, {"300e-2", 3}, {"12", 0},   {"1e1", 0},   {"\xE3\x80\x80" "2\xC2\xA0", 2},
      {"\xEF\xBC\x91", 0}, {"6.000", 6}, {"1e99999999999999999999", 0}, {"0x10", 0}, {"00004", 4},
      {"+0x3", 0},  {"0b", 0},     {"1_0", 0},      {"Infinity", 0}, {"0.5e+1", 5}, {"5E0", 5},
      {"+.5e1", 5}, {"0e0", 0},    {"0.0", 0},
  };
  for (size_t i = 0; i < sizeof levels / sizeof levels[0]; i++) {
    ob h = {0};
    ob_mark(&h, BDF_MARK_HEADING, levels[i].payload);
    ob_text(&h, "t", 0, 0, 1);
    t = extract(&h, NULL);
    CHECK(bdf_text_node_at(t, 0, &n) == BDF_OK && n.kind == BDF_NODE_HEADING);
    if (n.level != levels[i].level) {
      failures++;
      fprintf(stderr, "level of \"%s\" is %d, want %d\n", levels[i].payload, n.level, levels[i].level);
    }
    bdf_text_free(t);
  }

  /* cell references, and what is none */
  static const struct {
    const char *payload;
    const char *want;
  } cells[] = {
      {"A1", "cell:-1:c0,0,1,1"},
      {"b2", "cell:-1:c1,1,1,1"},
      {"AA10", "cell:-1:c9,26,1,1"},
      {"XFD1048576", "cell:-1:c1048575,16383,1,1"},
      {" A1 ", "cell:-1:c0,0,1,1"},
      {"A1 col", "cell:-1:c0,0,1,1:col"},
      {"A1\xC2\xA0\trow ", "cell:-1:c0,0,1,1:row"},
      {"B2:D3", "cell:-1:c1,1,2,3"},
      {"D3:B2", "cell:-1:c2,3,1,1"}, /* a range written backwards spans one */
      {"A0", "cell:-1"},             /* rows count from 1 */
      {"A1col", "cell:-1"},
      {"A1 column", "cell:-1"},
      {"A1 col row", "cell:-1"},
      {"A1:", "cell:-1"},
      {"1A", "cell:-1"},
      {"A", "cell:-1"},
      {"", "cell:-1"},
      {"A1 Col", "cell:-1"},
      {"A99999999999", "cell:-1:c4294967295,0,1,1"}, /* more than the type holds: the largest */
  };
  for (size_t i = 0; i < sizeof cells / sizeof cells[0]; i++) {
    ob c = {0};
    ob_mark(&c, BDF_MARK_CELL, cells[i].payload);
    ob_text(&c, "t", 0, 0, 1);
    t = extract(&c, NULL);
    expect_nodes(t, cells[i].want, __LINE__);
    bdf_text_free(t);
  }
}

static void test_nodes_containers(void) {
  /* lists: an item for each LIST_ITEM, one of its own for runs before the
   * first, lists inside items, and END closing the item with its list */
  ob o = {0};
  ob_mark(&o, BDF_MARK_LIST, "");
  ob_text(&o, "stray", 0, 0, 10);
  ob_mark(&o, BDF_MARK_LIST_ITEM, "");
  ob_text(&o, "one", 0, 10, 10);
  ob_mark(&o, BDF_MARK_LIST, "");
  ob_mark(&o, BDF_MARK_LIST_ITEM, "");
  ob_text(&o, "sub", 0, 20, 10);
  ob_mark(&o, BDF_MARK_END, "");
  ob_mark(&o, BDF_MARK_PARAGRAPH, "");
  ob_text(&o, "cont", 0, 30, 10);
  ob_mark(&o, BDF_MARK_END, "");
  ob_mark(&o, BDF_MARK_END, "");
  ob_text(&o, "after", 0, 40, 10);
  bdf_text *t = extract(&o, NULL);
  expect_nodes(t, "list:-1|item:0|par:1|item:0|par:3|list:3|item:5|par:6|par:3|par:-1", __LINE__);
  expect_run_nodes(t, "2,4,7,8,9", __LINE__);
  bdf_text_free(t);

  /* tables: a caption for runs before the first cell, cells that hold
   * leaves, and a sheet's cell after the table */
  ob tb = {0};
  ob_mark(&tb, BDF_MARK_TABLE, "");
  ob_text(&tb, "Caption", 0, 0, 10);
  ob_text(&tb, "still", 20, 0, 10);
  ob_mark(&tb, BDF_MARK_CELL, "A1 col");
  ob_text(&tb, "h", 0, 10, 10);
  ob_mark(&tb, BDF_MARK_CELL, "B1:C2");
  ob_mark(&tb, BDF_MARK_HEADING, "1");
  ob_text(&tb, "x", 0, 20, 10);
  ob_mark(&tb, BDF_MARK_CELL, "no place");
  ob_mark(&tb, BDF_MARK_END, "");
  ob_mark(&tb, BDF_MARK_CELL, "B12");
  ob_text(&tb, "sheet", 0, 30, 10);
  t = extract(&tb, NULL);
  expect_nodes(t, "table:-1|cap:0|cell:0:c0,0,1,1:col|par:2|cell:0:c0,1,2,2|head:4:l1|cell:0|cell:-1:c11,1,1,1",
               __LINE__);
  expect_run_nodes(t, "1,1,3,5,7", __LINE__);
  bdf_text_free(t);

  /* the structure runs on through the objects an object draws, and what is
   * open at the end of the top-level object is closed there */
  ob c = {0};
  ob_mark(&c, BDF_MARK_LIST_ITEM, "");
  ob_text(&c, "entry", 0, 0, 10);
  bdf_object *child = ob_decode(&c);
  bdf_object *table[2] = {NULL, child};
  objs children = {table, 2};
  ob top = {0};
  ob_mark(&top, BDF_MARK_LIST, "");
  ob_use(&top, ob_child(&top, 1));
  ob_use_at(&top, ob_child(&top, 1), 0, 20);
  ob_mark(&top, BDF_MARK_FIGURE, "left open");
  t = extract(&top, &children);
  expect_nodes(t, "list:-1|item:0|par:1|item:0|par:3|fig:3:alt=left open:b0,0,0,0", __LINE__);
  expect_run_nodes(t, "2,4", __LINE__);
  bdf_text_free(t);

  /* the language of runs, and the node a link belongs to */
  ob l = {0};
  ob_link(&l, 0, 0, 1, 1, "#page=1");
  ob_text(&l, "plain", 0, 0, 10);
  ob_mark(&l, BDF_MARK_LANG, "ja");
  ob_text(&l, "nihongo", 12, 0, 10);
  ob_link(&l, 0, 0, 1, 1, "#page=2");
  ob_mark(&l, BDF_MARK_LIST, "");
  ob_link(&l, 0, 0, 1, 1, "#page=3");
  ob_mark(&l, BDF_MARK_LANG, "");
  ob_mark(&l, BDF_MARK_LIST_ITEM, "");
  ob_text(&l, "item", 0, 20, 10);
  t = extract(&l, NULL);
  expect_nodes(t, "par:-1|list:-1|item:1|par:2", __LINE__);
  bdf_text_run r;
  bdf_text_link k;
  CHECK(bdf_text_run_at(t, 0, &r) == BDF_OK && r.lang_len == 0 && r.lang[0] == 0 && r.node == 0);
  CHECK(bdf_text_run_at(t, 1, &r) == BDF_OK && r.lang_len == 2 && memcmp(r.lang, "ja", 2) == 0);
  CHECK(bdf_text_run_at(t, 2, &r) == BDF_OK && r.lang_len == 0 && r.node == 3);
  CHECK(bdf_text_link_at(t, 0, &k) == BDF_OK && k.node == -1 && k.after == -1);
  CHECK(bdf_text_link_at(t, 1, &k) == BDF_OK && k.node == 0 && k.after == 1);
  CHECK(bdf_text_link_at(t, 2, &k) == BDF_OK && k.node == 1 && k.after == 1);
  bdf_text_free(t);
}

static void test_nodes_figures(void) {
  /* the bounds of a figure: what it draws, moved by the transform */
  ob o = {0};
  ob_translate(&o, 10, 20);
  ob_set_font(&o, ob_font(&o, "sans-serif"), 10);
  ob_mark(&o, BDF_MARK_FIGURE, "a chart");
  ob_rect(&o, BDF_OP_FILL_RECT, 0, 0, 30, 40);
  ob_text(&o, "label", 5, 50, 20); /* its advance wide, its size high, from 0.8 of it above */
  ob_rect(&o, BDF_OP_CLEAR_RECT, 0, 0, 500, 500); /* draws nothing */
  ob_mark(&o, BDF_MARK_END, "");
  ob_mark(&o, BDF_MARK_FIGURE, ""); /* a decoration that draws nothing: where it opens */
  ob_mark(&o, BDF_MARK_END, "");
  ob_rect(&o, BDF_OP_FILL_RECT, 0, 0, 900, 900); /* after the figures */
  bdf_text *t = extract(&o, NULL);
  expect_nodes(t, "fig:-1:alt=a chart:b10,20,30,52|par:0|fig:-1:alt=:b10,20,0,0", __LINE__);
  bdf_text_free(t);

  /* clipped by the clip at the time, which SAVE and RESTORE keep */
  ob c = {0};
  ob_mark(&c, BDF_MARK_FIGURE, "f");
  ob_op(&c, BDF_OP_SAVE);
  ob_rect(&c, BDF_OP_CLIP_RECT, 0, 0, 10, 10);
  ob_rect(&c, BDF_OP_FILL_RECT, 5, 5, 20, 20);
  ob_rect(&c, BDF_OP_STROKE_RECT, 50, 50, 1, 1); /* outside the clip */
  ob_op(&c, BDF_OP_RESTORE);
  ob_mark(&c, BDF_MARK_END, "");
  ob_mark(&c, BDF_MARK_FIGURE, "g");
  ob_rect(&c, BDF_OP_STROKE_RECT, 50, 50, 1, 1);
  t = extract(&c, NULL);
  expect_nodes(t, "fig:-1:alt=f:b5,5,5,5|fig:-1:alt=g:b50,50,1,1", __LINE__);
  bdf_text_free(t);

  /* paths by their points, figures inside figures, what a child draws, and
   * text without an advance: half its size for each UTF-16 unit, about its
   * anchor when centred */
  ob ch = {0};
  ch.bbox[2] = 8;
  ch.bbox[3] = 8;
  ob_rect(&ch, BDF_OP_FILL_RECT, 0, 0, 8, 8);
  bdf_object *child = ob_decode(&ch);
  bdf_object *table[2] = {NULL, child};
  objs children = {table, 2};
  ob p = {0};
  size_t sq = ob_rect_path(&p);
  ob_set_font(&p, ob_font(&p, "sans-serif"), 10);
  ob_mark(&p, BDF_MARK_FIGURE, "outer");
  ob_path_at(&p, sq, 100, 100);
  ob_mark(&p, BDF_MARK_FIGURE, "inner");
  ob_use_at(&p, ob_child(&p, 1), 200, 200);
  ob_mark(&p, BDF_MARK_END, "");
  ob_align(&p, BDF_ALIGN_CENTER);
  ob_text(&p, "e\xCC\x81\xF0\x9F\x98\x80", 50, 50, 0); /* e, a combining accent, an emoji: 4 units */
  ob_mark(&p, BDF_MARK_ALT_TEXT, "seen");
  /* drawn for the text: its box counts, and so does the text's, which is as
   * wide as the box and centred on its left edge */
  ob_use_at(&p, ob_child(&p, 1), 300, 0);
  t = extract(&p, &children);
  expect_nodes(t, "fig:-1:alt=outer:b40,-8,268,216|fig:0:alt=inner:b200,200,8,8|par:0", __LINE__);
  bdf_text_free(t);

  /* a mask is no content: neither its marks nor what it draws */
  ob m = {0};
  ob_mark(&m, BDF_MARK_FIGURE, "f");
  ob_mask_begin(&m);
  ob_rect(&m, BDF_OP_FILL_RECT, 0, 0, 100, 100);
  ob_mark(&m, BDF_MARK_LIST, "");
  ob_op(&m, BDF_OP_MASK_END);
  ob_rect(&m, BDF_OP_FILL_RECT, 1, 2, 3, 4);
  t = extract(&m, NULL);
  expect_nodes(t, "fig:-1:alt=f:b1,2,3,4", __LINE__);
  bdf_text_free(t);

  /* figures inside more figures than grow: the innermost keep no size */
  ob deep = {0};
  ob_translate(&deep, 3, 4);
  for (int i = 0; i < 70; i++) {
    ob_mark(&deep, BDF_MARK_FIGURE, "");
  }
  ob_rect(&deep, BDF_OP_FILL_RECT, 0, 0, 5, 6);
  t = extract(&deep, NULL);
  CHECK(bdf_text_node_count(t) == 70);
  for (size_t i = 0; i < 70; i++) {
    bdf_text_node n;
    CHECK(bdf_text_node_at(t, i, &n) == BDF_OK && n.kind == BDF_NODE_FIGURE && n.has_bounds);
    CHECK(n.parent == (int64_t)i - 1 && n.x == 3 && n.y == 4);
    CHECK(i < 64 ? (n.w == 5 && n.h == 6) : (n.w == 0 && n.h == 0));
  }
  bdf_text_free(t);
}

static void test_bad_references(void) {
  /* TestExtractTextBadReferences: references past the tables are not followed */
  ob o = {0};
  size_t s = ob_str(&o, "text");
  ob_child(&o, 1);
  ob_op(&o, BDF_OP_FONT);
  w_varuint(&o.ops, UINT64_MAX); /* a font that is not in the table */
  w_f32(&o.ops, 12);
  ob_op(&o, BDF_OP_FILL_TEXT);
  w_varuint(&o.ops, s);
  w_f32(&o.ops, 0);
  w_f32(&o.ops, 0);
  w_f32(&o.ops, 0);
  ob_use(&o, 7); /* an object that is not in the table */
  ob_op(&o, BDF_OP_MARK);
  w_u8(&o.ops, BDF_MARK_ALT_TEXT);
  w_varuint(&o.ops, s);
  ob_op(&o, BDF_OP_USE_AT);
  w_varuint(&o.ops, UINT64_MAX);
  w_f32(&o.ops, 0);
  w_f32(&o.ops, 0);
  bdf_object *obj = ob_decode(&o);
  bdf_text *t = NULL;
  bdf_error err;
  CHECK(bdf_text_extract(obj, none, NULL, &t, &err) == BDF_OK);
  if (t) {
    expect_runs(t, "text:0:0|text:1:1", __LINE__);
    bdf_text_run r;
    CHECK(bdf_text_run_at(t, 0, &r) == BDF_OK && !r.has_font && r.size == 12);
    bdf_text_free(t);
  }
  bdf_object_free(obj);

  /* a malformed op stream: the top-level object fails, with where */
  ob m = {0};
  ob_text(&m, "before", 0, 0, 10);
  ob_op(&m, 0x7E);
  obj = ob_decode(&m);
  CHECK(bdf_text_extract(obj, NULL, NULL, &t, &err) == BDF_ERR_FORMAT && t == NULL);
  CHECK_MSG(err, "unknown opcode");
  CHECK(err.offset == 14);

  /* the same damage in a child: its runs before it are kept, and the walk goes on */
  ob p = {0};
  size_t ref = ob_child(&p, 1);
  ob_text(&p, "a", 0, 0, 5);
  ob_use(&p, ref);
  ob_text(&p, "b", 0, 20, 5);
  bdf_object *parent = ob_decode(&p);
  bdf_object *table[] = {NULL, obj};
  objs res = {table, 2};
  CHECK(bdf_text_extract(parent, by_id, &res, &t, &err) == BDF_OK);
  if (t) {
    expect_runs(t, "a:0:0|before:0:0|b:1:0", __LINE__);
    bdf_text_free(t);
  }
  bdf_object_free(parent);
  bdf_object_free(obj);
}

/* nested builds objects that each draw the next one fan times, depth deep,
 * the last one drawing a text; object 0 is the first. */
static objs nested(size_t depth, size_t fan) {
  objs t;
  t.n = depth + 1;
  t.o = calloc(t.n, sizeof *t.o);
  ob leaf = {0};
  ob_text(&leaf, "x", 0, 0, 5);
  t.o[depth] = ob_decode(&leaf);
  for (size_t i = depth; i-- > 0;) {
    ob o = {0};
    size_t ref = ob_child(&o, (uint32_t)(i + 1));
    for (size_t k = 0; k < fan; k++) {
      ob_use(&o, ref);
    }
    t.o[i] = ob_decode(&o);
  }
  return t;
}

static void free_objs(objs *t) {
  for (size_t i = 0; i < t->n; i++) {
    bdf_object_free(t->o[i]);
  }
  free(t->o);
}

static void test_nesting(void) {
  /* TestExtractTextNesting */
  bdf_text *t = NULL;
  bdf_error err;
  objs shallow = nested(8, 2);
  CHECK(bdf_text_extract(shallow.o[0], by_id, &shallow, &t, &err) == BDF_OK && bdf_text_run_count(t) == 256);
  bdf_text_free(t);
  free_objs(&shallow);

  /* 64 objects deep is the most */
  objs most = nested(BDF_MAX_USE_DEPTH, 1);
  CHECK(bdf_text_extract(most.o[0], by_id, &most, &t, &err) == BDF_OK && bdf_text_run_count(t) == 1);
  bdf_text_free(t);
  free_objs(&most);
  objs deeper = nested(BDF_MAX_USE_DEPTH + 1, 1);
  CHECK(bdf_text_extract(deeper.o[0], by_id, &deeper, &t, &err) == BDF_ERR_FORMAT && t == NULL);
  CHECK_MSG(err, "objects draw objects too deep");
  free_objs(&deeper);
  objs deep = nested(20000, 1);
  CHECK(bdf_text_extract(deep.o[0], by_id, &deep, &t, &err) == BDF_ERR_FORMAT);
  CHECK_MSG(err, "objects draw objects too deep");
  free_objs(&deep);
}

static void test_masks_and_links(void) {
  ob c = {0};
  ob_text(&c, "child", 0, 0, 5);
  bdf_object *child = ob_decode(&c);

  ob o = {0};
  size_t ref = ob_child(&o, 1);
  ob_translate(&o, 10, 20);
  ob_link(&o, 1, 2, 3, 4, "#page=2"); /* before any run */
  ob_text(&o, "shown", 0, 0, 10);
  ob_mask_begin(&o); /* a soft mask is not content */
  ob_text(&o, "masked", 0, 0, 10);
  ob_link(&o, 0, 0, 1, 1, "https://masked.example/");
  ob_use(&o, ref);
  ob_mark(&o, BDF_MARK_ALT_TEXT, "masked alt");
  ob_translate(&o, 1000, 1000); /* nor do its transforms reach what follows */
  ob_op(&o, BDF_OP_MASK_END);
  ob_op(&o, BDF_OP_SAVE);
  ob_scale(&o, -2, 3);
  ob_link(&o, 1, 2, 3, 4, "https://example.com/a?b=c"); /* mirrored: the box is normalised */
  ob_op(&o, BDF_OP_RESTORE);
  ob_op(&o, BDF_OP_TRANSFORM); /* a quarter turn: (x, y) → (−y, x), then the translation */
  const float turn[6] = {0, 1, -1, 0, 0, 0};
  for (int i = 0; i < 6; i++) {
    w_f32(&o.ops, turn[i]);
  }
  ob_link(&o, 1, 2, 3, 4, "mailto:someone@example.com");
  ob_text(&o, "turned", 5, 0, 10);
  bdf_object *top = ob_decode(&o);
  bdf_object *table[] = {NULL, child};
  objs res = {table, 2};
  bdf_text *t = NULL;
  bdf_error err;
  CHECK(bdf_text_extract(top, by_id, &res, &t, &err) == BDF_OK);
  if (t) {
    expect_runs(t, "shown:0:0|turned:0:0", __LINE__); /* half a line lower: not a new line yet */
    bdf_text_run r;
    CHECK(bdf_text_run_at(t, 1, &r) == BDF_OK && r.x == 10 && r.y == 25);
    CHECK(r.matrix[0] == 0 && r.matrix[1] == 1 && r.matrix[2] == -1 && r.matrix[3] == 0 &&
          r.matrix[4] == 10 && r.matrix[5] == 20);
    bdf_text_link l;
    CHECK(bdf_text_link_count(t) == 3);
    CHECK(bdf_text_link_at(t, 0, &l) == BDF_OK && l.x == 11 && l.y == 22 && l.w == 3 && l.h == 4 &&
          l.after == -1 && strcmp(l.url, "#page=2") == 0 && l.url_len == 7);
    CHECK(bdf_text_link_at(t, 1, &l) == BDF_OK && l.x == 2 && l.y == 26 && l.w == 6 && l.h == 12 && l.after == 0);
    CHECK(bdf_link_parse(l.url, l.url_len, NULL) == BDF_LINK_URL);
    CHECK(bdf_text_link_at(t, 2, &l) == BDF_OK && l.x == 4 && l.y == 21 && l.w == 4 && l.h == 3 && l.after == 0);
    CHECK(bdf_text_link_at(t, 3, &l) == BDF_ERR_ARG);
    bdf_text_free(t);
  }
  bdf_object_free(top);
  bdf_object_free(child);
}

static void test_link_targets(void) {
  struct {
    const char *url;
    bdf_link_kind kind;
    int64_t page;
    const char *view; /* decoded */
  } cases[] = {
      {"#page=3", BDF_LINK_PAGE, 3, NULL},
      {"#page=9999999", BDF_LINK_PAGE, 9999999, NULL},
      {"#page=0", BDF_LINK_NONE, 0, NULL},
      {"#page=12345678", BDF_LINK_NONE, 0, NULL},
      {"#page=3x", BDF_LINK_NONE, 0, NULL},
      {"#page=", BDF_LINK_NONE, 0, NULL},
      {"#page=-1", BDF_LINK_NONE, 0, NULL},
      {"#view=a", BDF_LINK_VIEW, 0, "a"},
      {"#view=a&page=2", BDF_LINK_VIEW, 2, "a"},
      {"#view=a&page=0", BDF_LINK_NONE, 0, NULL},
      {"#view=a&x=1", BDF_LINK_NONE, 0, NULL},
      {"#view=a&page=2&x", BDF_LINK_NONE, 0, NULL},
      {"#view=", BDF_LINK_NONE, 0, NULL},
      {"#view=a#b", BDF_LINK_NONE, 0, NULL},
      {"#view=%E5%9B%B3+1", BDF_LINK_VIEW, 0, "\xe5\x9b\xb3 1"},
      {"#view=Sheet%201&page=4", BDF_LINK_VIEW, 4, "Sheet 1"},
      {"#view=a%2Bb", BDF_LINK_VIEW, 0, "a+b"},
      {"#view=%E5", BDF_LINK_NONE, 0, NULL},
      {"#view=%ZZ", BDF_LINK_NONE, 0, NULL},
      {"#view=%2", BDF_LINK_NONE, 0, NULL},
      {"#view=%C0%80", BDF_LINK_NONE, 0, NULL},
      {"#view=%ED%A0%80", BDF_LINK_NONE, 0, NULL},
      {"#view=%80", BDF_LINK_NONE, 0, NULL},
      {"#view=\xe5\x9b\xb3", BDF_LINK_VIEW, 0, "\xe5\x9b\xb3"},
      {"https://example.com/", BDF_LINK_URL, 0, NULL},
      {"HTTP://EXAMPLE.COM", BDF_LINK_URL, 0, NULL},
      {"http://example.com/#page=1", BDF_LINK_URL, 0, NULL},
      {"mailto:a@example.com", BDF_LINK_URL, 0, NULL},
      {"javascript:alert(1)", BDF_LINK_NONE, 0, NULL},
      {"ftp://example.com/", BDF_LINK_NONE, 0, NULL},
      {"file:///etc/passwd", BDF_LINK_NONE, 0, NULL},
      {" https://example.com/", BDF_LINK_NONE, 0, NULL},
      {"example.com", BDF_LINK_NONE, 0, NULL},
      {"#other", BDF_LINK_NONE, 0, NULL},
      {"#", BDF_LINK_NONE, 0, NULL},
      {"", BDF_LINK_NONE, 0, NULL},
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    bdf_link_target t;
    bdf_link_kind k = bdf_link_parse(cases[i].url, strlen(cases[i].url), &t);
    char id[64];
    int64_t n = bdf_link_view_id(&t, id, sizeof id);
    int ok = k == cases[i].kind && t.kind == k && t.page == cases[i].page;
    if (cases[i].view) {
      ok = ok && n == (int64_t)strlen(cases[i].view) && memcmp(id, cases[i].view, (size_t)n) == 0;
    } else {
      ok = ok && n == -1;
    }
    if (!ok) {
      failures++;
      fprintf(stderr, "link %s: kind %d page %lld id length %lld\n", cases[i].url, (int)k, (long long)t.page,
              (long long)n);
    }
  }
  bdf_link_target t;
  char small[2];
  CHECK(bdf_link_parse("#view=abc", 9, &t) == BDF_LINK_VIEW && t.view_len == 3);
  CHECK(bdf_link_view_id(&t, small, sizeof small) == -1); /* does not fit */
  CHECK(bdf_link_view_id(&t, small, 0) == -1 && bdf_link_view_id(NULL, small, 2) == -1);
  CHECK(bdf_link_parse(NULL, 0, &t) == BDF_LINK_NONE);
  /* the length counts, not a NUL */
  CHECK(bdf_link_parse("#page=12345", 7, &t) == BDF_LINK_PAGE && t.page == 1);
}

/* ---- Index parts --------------------------------------------------------- */


static wr text_index(void) {
  wr w = {0};
  w_bytes(&w, "BTXT", 4);
  w_u16(&w, 1);
  w_varuint(&w, 5);
  w_index_run(&w, 0, 0, 0, BDF_SEP_BREAK, "Hello");
  w_index_run(&w, 0, 0, 1, BDF_SEP_SPACE, "World");
  w_index_run(&w, 0, 1, 0, BDF_SEP_BREAK, "second");
  w_index_run(&w, 2, 0, 7, BDF_SEP_NONE, "-line");
  w_index_run(&w, 2, 0, 9, 9, ""); /* a separator no reader knows joins like none */
  return w;
}

static void test_text_index(void) {
  wr w = text_index();
  bdf_text_index *idx = NULL;
  bdf_error err;
  CHECK(bdf_text_index_decode(w.b, w.n, &idx, &err) == BDF_OK);
  if (idx) {
    bdf_index_run r;
    CHECK(bdf_text_index_count(idx) == 5);
    CHECK(bdf_text_index_run(idx, 3, &r) == BDF_OK && r.a == 2 && r.b == 0 && r.ordinal == 7 &&
          r.sep == BDF_SEP_NONE && strcmp(r.text, "-line") == 0 && r.text_len == 5);
    CHECK(bdf_text_index_run(idx, 5, &r) == BDF_ERR_ARG);
    char *plain = NULL;
    size_t n = 0;
    /* the separator of the first run does not show */
    CHECK(bdf_text_index_plain(idx, &plain, &n, &err) == BDF_OK && plain &&
          strcmp(plain, "Hello World\nsecond-line") == 0 && n == 23);
    bdf_free(plain);
    bdf_text_index_free(idx);
  }
  for (size_t cut = 0; cut < w.n; cut++) {
    if (bdf_text_index_decode(w.b, cut, &idx, &err) != BDF_ERR_FORMAT || idx != NULL) {
      failures++;
      fprintf(stderr, "a text index cut at %zu decoded\n", cut);
      bdf_text_index_free(idx);
    }
  }
  w.b[4] = 2;
  CHECK(bdf_text_index_decode(w.b, w.n, &idx, &err) == BDF_ERR_FORMAT);
  CHECK_MSG(err, "unsupported text index version");
  w.b[4] = 0; /* version 0 reads as version 1, as in the reference */
  CHECK(bdf_text_index_decode(w.b, w.n, &idx, &err) == BDF_OK);
  bdf_text_index_free(idx);
  w.b[0] = 'X';
  CHECK(bdf_text_index_decode(w.b, w.n, &idx, &err) == BDF_ERR_FORMAT);
  CHECK_MSG(err, "not a text index part");
  free(w.b);

  wr huge = {0};
  w_bytes(&huge, "BTXT", 4);
  w_u16(&huge, 1);
  w_varuint(&huge, (uint64_t)1 << 40);
  for (int i = 0; i < 64; i++) {
    w_u8(&huge, 0);
  }
  CHECK(bdf_text_index_decode(huge.b, huge.n, &idx, &err) == BDF_ERR_FORMAT);
  CHECK_MSG(err, "bad run count");
  free(huge.b);

  wr empty = {0};
  w_bytes(&empty, "BTXT", 4);
  w_u16(&empty, 1);
  w_varuint(&empty, 0);
  char *plain;
  size_t n;
  CHECK(bdf_text_index_decode(empty.b, empty.n, &idx, &err) == BDF_OK && bdf_text_index_count(idx) == 0);
  CHECK(bdf_text_index_plain(idx, &plain, &n, &err) == BDF_OK && n == 0 && plain[0] == '\0');
  bdf_free(plain);
  bdf_text_index_free(idx);
  free(empty.b);
  bdf_text_index_free(NULL);
}

static void test_cues(void) {
  wr w = {0};
  w_bytes(&w, "BCUE", 4);
  w_u16(&w, 1);
  w_varuint(&w, 2);
  w_varuint(&w, 0);
  w_f32(&w, 10);
  w_f32(&w, 20);
  w_f32(&w, 300);
  w_f32(&w, 40);
  w_varuint(&w, 3);
  w_f32(&w, 10);
  w_f32(&w, 80);
  w_f32(&w, 300);
  w_f32(&w, 40);
  w_varuint(&w, 3);
  w_varuint(&w, 0); /* tick 0 */
  w_varuint(&w, 0);
  w_f32(&w, 12);
  w_varuint(&w, 480); /* tick 480 */
  w_varuint(&w, 0);
  w_f32(&w, 150);
  w_varuint(&w, 480); /* tick 960, on the next system */
  w_varuint(&w, 1);
  w_f32(&w, 12);
  bdf_cues *c = NULL;
  bdf_error err;
  CHECK(bdf_cues_decode(w.b, w.n, &c, &err) == BDF_OK);
  if (c) {
    bdf_cue_system s;
    bdf_cue q;
    CHECK(bdf_cues_system_count(c) == 2 && bdf_cues_count(c) == 3);
    CHECK(bdf_cues_system(c, 1, &s) == BDF_OK && s.page == 3 && s.x == 10 && s.y == 80 && s.w == 300 && s.h == 40);
    CHECK(bdf_cues_cue(c, 1, &q) == BDF_OK && q.tick == 480 && q.system == 0 && q.x == 150);
    CHECK(bdf_cues_cue(c, 2, &q) == BDF_OK && q.tick == 960 && q.system == 1 && q.x == 12);
    CHECK(bdf_cues_system(c, 2, &s) == BDF_ERR_ARG && bdf_cues_cue(c, 3, &q) == BDF_ERR_ARG);
    bdf_cues_free(c);
  }
  for (size_t cut = 0; cut < w.n; cut++) {
    if (bdf_cues_decode(w.b, cut, &c, &err) != BDF_ERR_FORMAT || c != NULL) {
      failures++;
      fprintf(stderr, "a cue index cut at %zu decoded\n", cut);
      bdf_cues_free(c);
    }
  }
  /* the last cue names a system that is not there */
  w.b[w.n - 5] = 2;
  CHECK(bdf_cues_decode(w.b, w.n, &c, &err) == BDF_ERR_FORMAT);
  CHECK_MSG(err, "cue refers to a missing system");
  w.b[w.n - 5] = 1;
  w.b[4] = 2;
  CHECK(bdf_cues_decode(w.b, w.n, &c, &err) == BDF_ERR_FORMAT);
  CHECK_MSG(err, "unsupported cue index version");
  /* a text index is no cue index, and the other way round */
  wr ti = text_index();
  bdf_text_index *idx;
  CHECK(bdf_cues_decode(ti.b, ti.n, &c, &err) == BDF_ERR_FORMAT);
  CHECK_MSG(err, "not a cue index part");
  w.b[4] = 1;
  CHECK(bdf_text_index_decode(w.b, w.n, &idx, &err) == BDF_ERR_FORMAT);
  free(ti.b);
  free(w.b);
  bdf_cues_free(NULL);
}

/* ---- Documents ---------------------------------------------------------- */

static void expect_index(const bdf_text_index *idx, const char *want, int line) {
  char got[1024] = "";
  size_t k = 0;
  for (size_t i = 0; i < bdf_text_index_count(idx); i++) {
    bdf_index_run r;
    bdf_text_index_run(idx, i, &r);
    k += (size_t)snprintf(got + k, sizeof got - k, "%s%u,%u,%u,%d,%s", i ? "|" : "", r.a, r.b, r.ordinal, r.sep,
                          r.text);
  }
  if (strcmp(got, want) != 0) {
    failures++;
    fprintf(stderr, "line %d: index\n  %s\nwant\n  %s\n", line, got, want);
  }
}

static void expect_search(const bdf_search_text *st, const char *want, int line) {
  char got[2048] = "";
  size_t k = 0;
  for (size_t i = 0; i < bdf_search_text_view_count(st); i++) {
    bdf_search_view v;
    bdf_search_text_view(st, i, &v);
    k += (size_t)snprintf(got + k, sizeof got - k, "%s%s/%s/%s", i ? " || " : "", v.id, v.kind, v.title);
    for (size_t p = 0; p < v.npages; p++) {
      bdf_search_page pg;
      bdf_search_text_page(st, i, p, &pg);
      k += (size_t)snprintf(got + k, sizeof got - k, " [%lld:%s]", (long long)pg.page, pg.text);
      if (strlen(pg.text) != pg.text_len) {
        failures++;
        fprintf(stderr, "line %d: page %lld has a length of %zu\n", line, (long long)pg.page, pg.text_len);
      }
    }
  }
  if (strcmp(got, want) != 0) {
    failures++;
    fprintf(stderr, "line %d: search text\n  %s\nwant\n  %s\n", line, got, want);
  }
}

static void test_document(void) {
  docb d = {0};
  /* 1: a master that two pages draw; 2, 3, 4: the bodies of three pages */
  ob master = {0};
  ob_text(&master, "Master", 0, 0, 30);
  docb_object(&d, 1, &master);
  ob p1 = {0};
  size_t ref = ob_child(&p1, 1);
  ob_use_at(&p1, ref, 0, 500);
  ob_mark(&p1, BDF_MARK_PARAGRAPH, "");
  ob_text(&p1, "Hello", 0, 0, 30);
  ob_text(&p1, "World", 35, 0, 30);
  ob_link(&p1, 0, 0, 30, 10, "#view=sheet");
  docb_object(&d, 2, &p1);
  ob p2 = {0}; /* draws nothing that is text */
  ob_op(&p2, BDF_OP_SAVE);
  ob_op(&p2, BDF_OP_RESTORE);
  docb_object(&d, 3, &p2);
  ob p3 = {0}; /* white space around its text, some of it not ASCII */
  ob_text(&p3, " \t\xe3\x80\x80", 0, 0, 5);
  ob_mark(&p3, BDF_MARK_LINE, "");
  ob_text(&p3, "spaced\xc2\xa0", 0, 10, 30);
  ob_mark(&p3, BDF_MARK_PARAGRAPH, "");
  ob_text(&p3, "\xe2\x80\x89\n", 0, 20, 5);
  docb_object(&d, 4, &p3);
  /* 5, 6, 7: the tiles of a sheet of 100 units; each draws a run anchored in
   * it and runs that belong to its neighbours */
  const char *names[3] = {"a", "b", "c"};
  for (uint32_t i = 0; i < 3; i++) {
    ob t = {0};
    ob_text(&t, "right", 150, 10, 20); /* in the tile to the right */
    ob_text(&t, names[i], 10, 10, 20);
    ob_text(&t, "left", -5, 10, 20);
    ob_text(&t, "below", 10, 100, 20); /* the lower edge is the next tile's */
    ob_text(&t, "corner", 0, 0, 20);
    docb_object(&d, 5 + i, &t);
  }
  /* 8: a text index for the pages, which says something else than they do */
  wr idx = {0};
  w_bytes(&idx, "BTXT", 4);
  w_u16(&idx, 1);
  w_varuint(&idx, 3);
  w_index_run(&idx, 2, 0, 0, BDF_SEP_BREAK, "third");
  w_index_run(&idx, 0, 0, 0, BDF_SEP_BREAK, "stored");
  w_index_run(&idx, 0, 1, 0, BDF_SEP_SPACE, "index");
  docb_add(&d, 8, "idx", &idx);

  char n1[33], n2[33], n3[33], n4[33], n5[33], n6[33], n7[33], n8[33], views[4096];
  part_name(1, n1);
  part_name(2, n2);
  part_name(3, n3);
  part_name(4, n4);
  part_name(5, n5);
  part_name(6, n6);
  part_name(7, n7);
  part_name(8, n8);
  snprintf(views, sizeof views,
           "[{\"id\":\"pages\",\"kind\":\"fixed\",\"title\":\"Pages\",\"pages\":["
           "{\"w\":100,\"h\":100,\"layers\":[{\"role\":\"master\",\"obj\":\"%s\"},{\"role\":\"body\",\"obj\":\"%s\"}]},"
           "{\"w\":100,\"h\":100,\"layers\":[{\"role\":\"body\",\"obj\":\"%s\"}]},"
           "{\"w\":100,\"h\":100,\"layers\":[{\"role\":\"master\",\"obj\":\"%s\"},{\"role\":\"body\",\"obj\":\"%s\"}]}]},"
           "{\"id\":\"indexed\",\"kind\":\"flow\",\"title\":\"\",\"textIndex\":\"%s\",\"pages\":["
           "{\"w\":100,\"h\":100,\"layers\":[{\"role\":\"body\",\"obj\":\"%s\"}]}]},"
           "{\"id\":\"scroll\",\"kind\":\"scroll\",\"pages\":[{\"w\":100,\"h\":100,\"layers\":[{\"role\":\"body\",\"obj\":\"%s\"}]}]},"
           "{\"id\":\"sheet\",\"kind\":\"sheet\",\"title\":\"Sheet\",\"tile\":100,"
           "\"tiles\":{\"1,0\":\"%s\",\"0,1\":\"%s\",\"0,0\":\"%s\"}},"
           "{\"id\":\"empty\",\"kind\":\"fixed\"}]",
           n1, n2, n3, n1, n4, n8, n2, n2, n5, n7, n6);
  wr keep;
  bdf_doc *doc = docb_open(&d, views, &keep);
  if (!doc) {
    free(keep.b);
    return;
  }
  bdf_error err;

  /* the text of a top-level object, with what it draws read from the document */
  uint8_t h[16];
  bdf_hash_parse(n2, 32, h);
  bdf_text *t = NULL;
  CHECK(bdf_doc_text(doc, h, &t, &err) == BDF_OK);
  if (t) {
    expect_runs(t, "Master:0:0|Hello:2:0|World:1:0", __LINE__);
    bdf_text_run r;
    CHECK(bdf_text_run_at(t, 0, &r) == BDF_OK && r.y == 500);
    bdf_text_link l;
    bdf_link_target target;
    char id[16];
    CHECK(bdf_text_link_count(t) == 1 && bdf_text_link_at(t, 0, &l) == BDF_OK && l.after == 2);
    CHECK(bdf_link_parse(l.url, l.url_len, &target) == BDF_LINK_VIEW);
    int64_t n = bdf_link_view_id(&target, id, sizeof id);
    CHECK(n == 5 && bdf_doc_view_find(doc, id, (size_t)n) == 3);
    bdf_text_free(t);
  }
  CHECK(bdf_doc_view_find(doc, "pages", 5) == 0 && bdf_doc_view_find(doc, "page", 4) == -1);
  bdf_hash_parse(n8, 32, h); /* a part that is no object */
  CHECK(bdf_doc_text(doc, h, &t, &err) == BDF_ERR_FORMAT && t == NULL);
  CHECK_MSG(err, "object missing or invalid");
  memset(h, 0xEE, sizeof h); /* a part that is not there */
  CHECK(bdf_doc_text(doc, h, &t, &err) == BDF_ERR_FORMAT);
  bdf_object *obj = NULL;
  CHECK(bdf_doc_object(doc, h, &obj, &err) == BDF_ERR_FORMAT && obj == NULL);
  bdf_hash_parse(n1, 32, h);
  CHECK(bdf_doc_object(doc, h, &obj, &err) == BDF_OK && bdf_object_string_count(obj) == 1);
  bdf_object_free(obj);

  /* the text of the views */
  bdf_text_index *idx2 = NULL;
  CHECK(bdf_doc_view_text(doc, 0, 0, &idx2, &err) == BDF_OK);
  if (idx2) {
    /* the first run of every object starts a paragraph; the master's run
     * comes again with the page that draws it */
    expect_index(idx2, "0,0,0,2,Master|0,1,0,2,Master|0,1,1,2,Hello|0,1,2,1,World|2,0,0,2,Master|2,1,0,2, \t\xe3\x80\x80|"
                       "2,1,1,1,spaced\xc2\xa0|2,1,2,2,\xe2\x80\x89\n",
                 __LINE__);
    bdf_text_index_free(idx2);
  }
  CHECK(bdf_doc_view_text(doc, 1, 0, &idx2, &err) == BDF_OK);
  if (idx2) {
    expect_index(idx2, "2,0,0,2,third|0,0,0,2,stored|0,1,0,1,index", __LINE__); /* the stored index */
    bdf_text_index_free(idx2);
  }
  CHECK(bdf_doc_view_text(doc, 1, 1, &idx2, &err) == BDF_OK);
  if (idx2) {
    expect_index(idx2, "0,0,0,2,Master|0,0,1,2,Hello|0,0,2,1,World", __LINE__); /* extracted all the same */
    bdf_text_index_free(idx2);
  }
  CHECK(bdf_doc_view_text(doc, 3, 0, &idx2, &err) == BDF_OK);
  if (idx2) {
    /* tiles by row and then column; a tile keeps the runs anchored in it:
     * its left and upper edges are its own, the others its neighbours' */
    expect_index(idx2, "0,0,1,2,b|0,0,4,1,corner|1,0,1,2,a|1,0,4,1,corner|0,1,1,2,c|0,1,4,1,corner", __LINE__);
    bdf_text_index_free(idx2);
  }
  CHECK(bdf_doc_view_text(doc, 4, 0, &idx2, &err) == BDF_OK && bdf_text_index_count(idx2) == 0);
  bdf_text_index_free(idx2);
  CHECK(bdf_doc_view_text(doc, 5, 0, &idx2, &err) == BDF_ERR_ARG);

  /* the text for a search index: pages without text are left out, a scroll
   * view beside a flow view too, and the text is trimmed */
  bdf_search_text *st = NULL;
  CHECK(bdf_doc_search_text(doc, &st, &err) == BDF_OK);
  if (st) {
    expect_search(st, "pages/fixed/Pages [1:Master\nMaster\nHello World] [3:Master\n \t\xe3\x80\x80 spaced] || "
                      "indexed/flow/ [1:stored index] [3:third] || sheet/sheet/Sheet [0:b corner\na corner\nc corner] || "
                      "empty/fixed/",
                  __LINE__);
    bdf_search_view v;
    bdf_search_page pg;
    CHECK(bdf_search_text_view(st, 4, &v) == BDF_ERR_ARG && bdf_search_text_page(st, 0, 2, &pg) == BDF_ERR_ARG);
    bdf_search_text_free(st);
  }
  bdf_search_text_free(NULL);
  bdf_doc_free(doc);
  free(keep.b);
}

static void test_document_errors(void) {
  bdf_error err;
  bdf_text_index *idx = NULL;
  wr keep;

  /* a layer whose object is not there, and a tile key that is no "x,y" */
  docb d = {0};
  ob o = {0};
  ob_text(&o, "x", 0, 0, 5);
  docb_object(&d, 1, &o);
  char n1[33], views[1024];
  part_name(1, n1);
  snprintf(views, sizeof views,
           "[{\"id\":\"p\",\"kind\":\"fixed\",\"pages\":[{\"w\":1,\"h\":1,\"layers\":[{\"role\":\"body\","
           "\"obj\":\"99999999999999999999999999999999\"}]}]},"
           "{\"id\":\"s1\",\"kind\":\"sheet\",\"tiles\":{\"01,0\":\"%s\"}},"
           "{\"id\":\"s2\",\"kind\":\"sheet\",\"tiles\":{\"0,0\":\"nothex\"}},"
           "{\"id\":\"s3\",\"kind\":\"sheet\",\"tiles\":{\"0,0\":\"%s\",\"0,0\":\"%s\",\"4294967297,0\":\"%s\"}},"
           "{\"id\":\"f\",\"kind\":\"flow\",\"textIndex\":\"%s\",\"pages\":[]}]",
           n1, n1, n1, n1, n1);
  bdf_doc *doc = docb_open(&d, views, &keep);
  if (doc) {
    CHECK(bdf_doc_view_text(doc, 0, 0, &idx, &err) == BDF_ERR_FORMAT && idx == NULL);
    CHECK_MSG(err, "object missing or invalid");
    CHECK(bdf_doc_view_text(doc, 1, 0, &idx, &err) == BDF_ERR_FORMAT);
    CHECK_MSG(err, "bad tile key");
    CHECK(bdf_doc_view_text(doc, 2, 0, &idx, &err) == BDF_ERR_FORMAT);
    CHECK_MSG(err, "bad part name");
    /* of two members with one name the last counts; a tile number beyond
     * 32 bits is kept as the reference keeps it */
    bdf_view v;
    CHECK(bdf_doc_view(doc, 3, &v) == BDF_OK && v.ntiles == 2);
    CHECK(bdf_doc_view_text(doc, 3, 0, &idx, &err) == BDF_OK);
    if (idx) {
      expect_index(idx, "0,0,0,2,x|1,0,0,2,x", __LINE__);
      bdf_text_index_free(idx);
    }
    /* a text index that is none: the view has no text to give */
    CHECK(bdf_doc_view_text(doc, 4, 0, &idx, &err) == BDF_ERR_FORMAT);
    CHECK_MSG(err, "not a text index part");
    CHECK(bdf_doc_view_text(doc, 4, 1, &idx, &err) == BDF_OK && bdf_text_index_count(idx) == 0);
    bdf_text_index_free(idx);
    bdf_search_text *st = NULL;
    CHECK(bdf_doc_search_text(doc, &st, &err) == BDF_ERR_FORMAT && st == NULL);
    bdf_doc_free(doc);
  }
  free(keep.b);
  CHECK(bdf_doc_search_text(NULL, NULL, &err) == BDF_ERR_ARG);
}

int main(void) {
  test_extract();
  test_structure_marks();
  test_nodes_leaves();
  test_nodes_containers();
  test_nodes_figures();
  test_bad_references();
  test_nesting();
  test_masks_and_links();
  test_link_targets();
  test_text_index();
  test_cues();
  test_document();
  test_document_errors();
  return finish();
}
