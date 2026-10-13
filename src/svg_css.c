/*
 * The style sheets of an SVG document: a port of css.go of the reference's
 * package image/svg. What is read of a style sheet is what images made for
 * previews use: rules with selectors of type, class and id, combined and
 * inside each other. Rules with attribute selectors or pseudo-classes and
 * at-rules are left out.
 *
 * The rules, the order they win in and the count of the rules matched
 * against elements are those of the Go code. What an element is given is
 * kept differently: the Go code makes a map for every element, here the
 * declarations of a rule are sorted once, an element that one rule matches
 * shares them, and one that several match gets them merged. Elements with
 * the same rules in a row share the result.
 */
#include <stdlib.h>
#include <string.h>

#include "svg_internal.h"

/* ---- Declarations --------------------------------------------------------- */

static int bdfi_svg_decl_cmp(const void *a, const void *b) {
  return bdfi_svg_str_cmp(((const bdfi_svg_decl *)a)->name, ((const bdfi_svg_decl *)b)->name);
}

/* strings.Split(s, sep), a piece at a time: the piece that starts at *pos,
 * with *pos moved past it and its separator; 0 after the last. */
static int bdfi_svg_next_piece(bdfi_svg_str s, char sep, size_t *pos, bdfi_svg_str *piece) {
  if (*pos > s.n) {
    return 0;
  }
  const char *p = *pos < s.n ? memchr(s.p + *pos, sep, s.n - *pos) : NULL;
  size_t end = p ? (size_t)(p - s.p) : s.n;
  *piece = bdfi_svg_str_of(s.p + *pos, end - *pos);
  *pos = end + 1;
  return 1;
}

/* parseDecls in css.go: reads CSS declarations ("fill: red; stroke: blue")
 * in their order, into an array the caller frees. The names are in lower
 * case, in s where they were, in the pool where they were not. */
static int bdfi_svg_parse_decls(bdfi_svg_pool *pool, bdfi_svg_str s, bdfi_svg_decl **out,
                                size_t *count) {
  size_t n = 0, pos = 0;
  bdfi_svg_str d;
  while (bdfi_svg_next_piece(s, ';', &pos, &d)) {
    if (d.n > 0 && memchr(d.p, ':', d.n)) {
      n++;
    }
  }
  bdfi_svg_decl *decls = bdfi_malloc_array(n, sizeof *decls);
  if (!decls) {
    return -1;
  }
  n = 0;
  pos = 0;
  while (bdfi_svg_next_piece(s, ';', &pos, &d)) {
    /* k, v, ok := strings.Cut(d, ":") */
    const char *colon = d.n > 0 ? memchr(d.p, ':', d.n) : NULL;
    if (!colon) {
      continue;
    }
    bdfi_svg_str k = bdfi_svg_trim_space(bdfi_svg_str_of(d.p, (size_t)(colon - d.p)));
    bdfi_svg_str v =
        bdfi_svg_trim_space(bdfi_svg_str_of(colon + 1, d.n - (size_t)(colon - d.p) - 1));
    if (v.n >= 10 && memcmp(v.p + v.n - 10, "!important", 10) == 0) {
      v.n -= 10;
      v = bdfi_svg_trim_space(v);
    }
    /* strings.ToLower(k): the name itself when it has no capital letter */
    int same = 1;
    for (size_t i = 0; i < k.n && same; i++) {
      same = (uint8_t)k.p[i] < 0x80 && !(k.p[i] >= 'A' && k.p[i] <= 'Z');
    }
    if (!same) {
      size_t len = bdfi_svg_lower(k.p, k.n, NULL);
      char *lower = bdfi_svg_pool_bytes(pool, len);
      if (!lower) {
        free(decls);
        return -1;
      }
      (void)bdfi_svg_lower(k.p, k.n, lower);
      k = bdfi_svg_str_of(lower, len);
    }
    decls[n].name = k;
    decls[n].value = v;
    n++;
  }
  *out = decls;
  *count = n;
  return 0;
}

/* Sorts declarations by name and keeps one of each name: the first that was
 * written (what Node.Declared finds in a style attribute), or the last
 * (what is left in the map a rule's declarations are put into). */
static int bdfi_svg_unique_decls(bdfi_svg_decl *d, size_t *count, int keep_last) {
  if (bdfi_svg_sort(d, *count, sizeof *d, bdfi_svg_decl_cmp) != 0) {
    return -1;
  }
  size_t n = 0;
  for (size_t i = 0; i < *count; i++) {
    if (n > 0 && bdfi_svg_str_cmp(d[n - 1].name, d[i].name) == 0) {
      if (keep_last) {
        d[n - 1] = d[i];
      }
    } else {
      d[n++] = d[i];
    }
  }
  *count = n;
  return 0;
}

int bdfi_svg_style_decls(bdfi_svg_pool *pool, bdfi_svg_str s, const bdfi_svg_decl **out,
                         size_t *count) {
  bdfi_svg_decl *decls;
  size_t n;
  *out = NULL;
  *count = 0;
  if (bdfi_svg_parse_decls(pool, s, &decls, &n) != 0) {
    return -1;
  }
  bdfi_svg_decl *kept = NULL;
  if (bdfi_svg_unique_decls(decls, &n, 0) == 0 && n > 0) {
    kept = bdfi_svg_pool_array(pool, n, sizeof *kept);
    if (kept) {
      memcpy(kept, decls, n * sizeof *kept);
    }
  }
  free(decls);
  if (n > 0 && !kept) {
    return -1;
  }
  *out = kept;
  *count = n;
  return 0;
}

/* Node.Declared in css.go. */
int bdfi_svg_declared(const bdfi_svg_node *n, const char *name, size_t len, bdfi_svg_str *out) {
  const bdfi_svg_decl *d = bdfi_svg_find(n->style, n->nstyle, name, len);
  if (d) {
    *out = d->value;
    return 1;
  }
  if (bdfi_svg_sheet(n, name, len, out)) {
    return 1;
  }
  d = bdfi_svg_find(n->attrs, n->nattrs, name, len);
  if (d) {
    *out = bdfi_svg_trim_space(d->value);
    return 1;
  }
  *out = bdfi_svg_str_of("", 0);
  return 0;
}

/* ---- Rules ---------------------------------------------------------------- */

/* cssCompound: a simple selector sequence: tag, #id and .classes. The
 * classes are kept once each, sorted: an element has all of them or it does
 * not, however often the selector names one. */
typedef struct bdfi_svg_compound {
  bdfi_svg_str tag, id;
  bdfi_svg_str first_class; /* classes[0] of the reference: the key of the rule */
  bdfi_svg_str *classes;
  size_t nclasses;
} bdfi_svg_compound;

static int bdfi_svg_str_cmp_of(const void *a, const void *b) {
  return bdfi_svg_str_cmp(*(const bdfi_svg_str *)a, *(const bdfi_svg_str *)b);
}

/* The declarations of a rule, sorted by name, the last of a name: what its
 * rules give an element. The selectors of one rule share them. */
typedef struct bdfi_svg_block {
  const bdfi_svg_decl **ptrs;
  size_t n;
} bdfi_svg_block;

/* cssRule: a rule of a style sheet with one selector. */
typedef struct bdfi_svg_rule {
  bdfi_svg_compound *sel; /* descendant combinators between them */
  size_t nsel;
  int64_t spec;
  size_t order;
  const bdfi_svg_block *block;
} bdfi_svg_rule;

/* What the last part of a rule's selector asks for: the rules of styleSheet
 * by id, by class, by tag, and the others. */
enum { BDFI_SVG_BY_ID, BDFI_SVG_BY_CLASS, BDFI_SVG_BY_TAG, BDFI_SVG_OTHERS };

typedef struct bdfi_svg_keyed {
  int kind;
  bdfi_svg_str key;
  const bdfi_svg_rule *rule;
} bdfi_svg_keyed;

/* The rules of one key, in their order: keyed[first .. first+count). */
typedef struct bdfi_svg_group {
  int kind;
  bdfi_svg_str key;
  size_t first, count;
  uint64_t stamp; /* the element its rules were last matched against */
} bdfi_svg_group;

/* styleSheet, and what applying it needs. */
typedef struct bdfi_svg_css {
  bdfi_svg_pool *pool;
  bdfi_arena tmp; /* selectors, which are not kept */
  bdfi_svg_rule *rules;
  size_t nrules, rules_cap;
  bdfi_svg_keyed *keyed;
  bdfi_svg_group *groups;
  size_t ngroups;
  int64_t matches, most; /* the rules matched against elements, and the most there may be */
  const bdfi_svg_rule **hits, **last_hits;
  size_t nhits, hits_cap, nlast, last_cap;
  const bdfi_svg_decl *const *last_sheet;
  size_t last_n;
  const bdfi_svg_decl **merge[2];
  size_t merge_cap;
  size_t stored; /* declarations kept for elements */
  uint64_t work; /* declarations gone through */
  uint64_t visit;
  bdf_status status;
} bdfi_svg_css;

static int bdfi_svg_sel_sep(uint32_t r) {
  return r == '>' || bdfi_svg_is_space(r);
}

/* strings.Fields(strings.ReplaceAll(s, ">", " ")), a field at a time. */
static int bdfi_svg_next_part(bdfi_svg_str s, size_t *pos, bdfi_svg_str *part) {
  size_t i = *pos, len;
  while (i < s.n && bdfi_svg_sel_sep(bdfi_svg_rune(s.p, s.n, i, &len))) {
    i += len;
  }
  if (i >= s.n) {
    *pos = s.n;
    return 0;
  }
  size_t start = i;
  while (i < s.n && !bdfi_svg_sel_sep(bdfi_svg_rune(s.p, s.n, i, &len))) {
    i += len;
  }
  *part = bdfi_svg_str_of(s.p + start, i - start);
  *pos = i;
  return 1;
}

/* read in parseSelector: a name, up to what starts the next thing. */
static bdfi_svg_str bdfi_svg_sel_read(bdfi_svg_str part, size_t *i) {
  size_t j = *i;
  while (j < part.n && part.p[j] != '.' && part.p[j] != '#' && part.p[j] != '[' &&
         part.p[j] != ':') {
    j++;
  }
  bdfi_svg_str v = bdfi_svg_str_of(part.p + *i, j - *i);
  *i = j;
  return v;
}

/* parseSelector in css.go: 1 and the selector, 0 for one that is left out
 * (attribute selectors and pseudo-classes), -1 when out of memory. A
 * selector of combinators alone is taken, with no parts. */
static int bdfi_svg_parse_selector(bdfi_svg_css *css, bdfi_svg_str s, bdfi_svg_rule *rule) {
  rule->sel = NULL;
  rule->nsel = 0;
  rule->spec = 0;
  if (s.n == 0) {
    return 0;
  }
  size_t pos = 0, nparts = 0;
  bdfi_svg_str part;
  while (bdfi_svg_next_part(s, &pos, &part)) {
    nparts++;
  }
  if (nparts == 0) {
    return 1;
  }
  if (nparts > SIZE_MAX / sizeof *rule->sel) {
    return -1;
  }
  rule->sel = bdfi_arena_alloc(&css->tmp, nparts * sizeof *rule->sel);
  if (!rule->sel) {
    return -1;
  }
  pos = 0;
  while (bdfi_svg_next_part(s, &pos, &part)) {
    bdfi_svg_compound *c = &rule->sel[rule->nsel++];
    size_t i = 0, dots = 0;
    c->tag = c->id = bdfi_svg_str_of("", 0);
    for (size_t k = 0; k < part.n; k++) {
      dots += part.p[k] == '.' ? 1u : 0u;
    }
    if (dots > 0) {
      c->classes = bdfi_arena_alloc(&css->tmp, dots * sizeof *c->classes);
      if (!c->classes) {
        return -1;
      }
    }
    if (part.p[0] != '.' && part.p[0] != '#') {
      c->tag = bdfi_svg_sel_read(part, &i);
      if (bdfi_svg_str_eq(c->tag, "*", 1)) {
        c->tag.n = 0;
      } else {
        rule->spec++;
      }
    }
    while (i < part.n) {
      if (part.p[i] == '.') {
        i++;
        c->classes[c->nclasses++] = bdfi_svg_sel_read(part, &i);
        rule->spec += 100;
      } else if (part.p[i] == '#') {
        i++;
        c->id = bdfi_svg_sel_read(part, &i);
        rule->spec += 10000;
      } else {
        return 0; /* attribute selectors and pseudo-classes */
      }
    }
    if (c->nclasses > 0) {
      c->first_class = c->classes[0];
      if (bdfi_svg_sort(c->classes, c->nclasses, sizeof *c->classes, bdfi_svg_str_cmp_of) != 0) {
        return -1;
      }
      size_t kept = 1;
      for (size_t k = 1; k < c->nclasses; k++) {
        if (bdfi_svg_str_cmp(c->classes[kept - 1], c->classes[k]) != 0) {
          c->classes[kept++] = c->classes[k];
        }
      }
      c->nclasses = kept;
    }
  }
  return 1;
}

/* The declarations of a rule: parseDecls, and what the map they are put
 * into in their order keeps. */
static const bdfi_svg_block *bdfi_svg_parse_block(bdfi_svg_css *css, bdfi_svg_str body) {
  bdfi_svg_decl *decls;
  size_t n;
  bdfi_svg_block *block = bdfi_arena_alloc(&css->tmp, sizeof *block);
  if (!block || bdfi_svg_parse_decls(css->pool, body, &decls, &n) != 0) {
    return NULL;
  }
  if (bdfi_svg_unique_decls(decls, &n, 1) != 0) {
    free(decls);
    return NULL;
  }
  bdfi_svg_decl *kept = bdfi_svg_pool_array(css->pool, n, sizeof *kept);
  block->ptrs = bdfi_svg_pool_array(css->pool, n, sizeof *block->ptrs);
  if (!kept || !block->ptrs) {
    free(decls);
    return NULL;
  }
  for (size_t i = 0; i < n; i++) {
    kept[i] = decls[i];
    block->ptrs[i] = &kept[i];
  }
  block->n = n;
  free(decls);
  return block;
}

/*
 * parseCSS in css.go: reads the rules of a style sheet: compound selectors
 * of type, id and class, and descendant combinators. At-rules are skipped.
 * src is the sheet without its comments, kept as long as the document.
 * It returns BDF_ERR_FORMAT for a rule whose selector has no parts: the
 * reference takes it and then panics over it (newStyleSheet indexes the
 * last part of every selector).
 */
static bdf_status bdfi_svg_parse_rules(bdfi_svg_css *css, bdfi_svg_str src) {
  size_t pos = 0, order = 0;
  for (;;) {
    const char *brace = pos < src.n ? memchr(src.p + pos, '{', src.n - pos) : NULL;
    if (!brace) {
      break;
    }
    size_t open = (size_t)(brace - src.p);
    bdfi_svg_str prelude = bdfi_svg_trim_space(bdfi_svg_str_of(src.p + pos, open - pos));
    /* find the matching close brace */
    size_t depth = 0, end = SIZE_MAX;
    for (size_t k = open; k < src.n; k++) {
      if (src.p[k] == '{') {
        depth++;
      } else if (src.p[k] == '}') {
        depth--;
      }
      if (depth == 0) {
        end = k;
        break;
      }
    }
    if (end == SIZE_MAX) {
      break;
    }
    bdfi_svg_str body = bdfi_svg_str_of(src.p + open + 1, end - open - 1);
    pos = end + 1;
    if (prelude.n > 0 && prelude.p[0] == '@') {
      continue;
    }
    const bdfi_svg_block *block = NULL;
    size_t at = 0;
    bdfi_svg_str s;
    while (bdfi_svg_next_piece(prelude, ',', &at, &s)) {
      if (css->nrules == css->rules_cap) {
        size_t cap = css->rules_cap ? css->rules_cap * 2 : 16;
        bdfi_svg_rule *p =
            cap > css->rules_cap ? bdfi_realloc_array(css->rules, cap, sizeof *p) : NULL;
        if (!p) {
          return BDF_ERR_NOMEM;
        }
        css->rules = p;
        css->rules_cap = cap;
      }
      bdfi_svg_rule *rule = &css->rules[css->nrules];
      int ok = bdfi_svg_parse_selector(css, bdfi_svg_trim_space(s), rule);
      if (ok < 0) {
        return BDF_ERR_NOMEM;
      }
      if (!ok) {
        continue;
      }
      if (rule->nsel == 0) {
        return BDF_ERR_FORMAT; /* Go panics here: index out of range [-1] */
      }
      if (!block) {
        block = bdfi_svg_parse_block(css, body);
        if (!block) {
          return BDF_ERR_NOMEM;
        }
      }
      rule->block = block;
      rule->order = order++;
      css->nrules++;
    }
  }
  return BDF_OK;
}

static int bdfi_svg_keyed_cmp(const void *a, const void *b) {
  const bdfi_svg_keyed *x = a, *y = b;
  if (x->kind != y->kind) {
    return x->kind < y->kind ? -1 : 1;
  }
  return bdfi_svg_str_cmp(x->key, y->key);
}

/* newStyleSheet in css.go: the rules by what the last part of their
 * selector asks for: an id, its first class, a tag, or none of them. */
static int bdfi_svg_index_rules(bdfi_svg_css *css) {
  css->keyed = bdfi_malloc_array(css->nrules, sizeof *css->keyed);
  if (!css->keyed) {
    return -1;
  }
  for (size_t i = 0; i < css->nrules; i++) {
    const bdfi_svg_rule *r = &css->rules[i];
    const bdfi_svg_compound *last = &r->sel[r->nsel - 1];
    bdfi_svg_keyed *k = &css->keyed[i];
    k->rule = r;
    if (last->id.n != 0) {
      k->kind = BDFI_SVG_BY_ID;
      k->key = last->id;
    } else if (last->nclasses > 0) {
      k->kind = BDFI_SVG_BY_CLASS;
      k->key = last->first_class;
    } else if (last->tag.n != 0) {
      k->kind = BDFI_SVG_BY_TAG;
      k->key = last->tag;
    } else {
      k->kind = BDFI_SVG_OTHERS;
      k->key = bdfi_svg_str_of("", 0);
    }
  }
  /* the sort is stable: the rules of a key stay in their order */
  if (bdfi_svg_sort(css->keyed, css->nrules, sizeof *css->keyed, bdfi_svg_keyed_cmp) != 0) {
    return -1;
  }
  size_t keys = 0;
  for (size_t i = 0; i < css->nrules; i++) {
    keys += i == 0 || bdfi_svg_keyed_cmp(&css->keyed[i - 1], &css->keyed[i]) != 0 ? 1u : 0u;
  }
  css->groups = bdfi_malloc_array(keys, sizeof *css->groups);
  if (!css->groups) {
    return -1;
  }
  for (size_t i = 0; i < css->nrules; i++) {
    if (i > 0 && bdfi_svg_keyed_cmp(&css->keyed[i - 1], &css->keyed[i]) == 0) {
      css->groups[css->ngroups - 1].count++;
      continue;
    }
    bdfi_svg_group *g = &css->groups[css->ngroups++];
    g->kind = css->keyed[i].kind;
    g->key = css->keyed[i].key;
    g->first = i;
    g->count = 1;
    g->stamp = 0;
  }
  return 0;
}

static bdfi_svg_group *bdfi_svg_find_group(bdfi_svg_css *css, int kind, bdfi_svg_str key) {
  size_t lo = 0, hi = css->ngroups;
  while (lo < hi) {
    size_t m = lo + (hi - lo) / 2;
    bdfi_svg_group *g = &css->groups[m];
    int c = g->kind != kind ? (g->kind < kind ? -1 : 1) : bdfi_svg_str_cmp(g->key, key);
    if (c == 0) {
      return g;
    }
    if (c < 0) {
      lo = m + 1;
    } else {
      hi = m;
    }
  }
  return NULL;
}

/* cssCompound.matches in css.go. */
static int bdfi_svg_compound_matches(const bdfi_svg_compound *c, const bdfi_svg_node *n) {
  bdfi_svg_str v;
  if (c->tag.n != 0 && bdfi_svg_str_cmp(c->tag, n->name) != 0) {
    return 0;
  }
  if (c->id.n != 0 && (!bdfi_svg_attr(n, "id", 2, &v) || bdfi_svg_str_cmp(v, c->id) != 0)) {
    return 0;
  }
  if (c->nclasses > 0) {
    (void)bdfi_svg_attr(n, "class", 5, &v);
    for (size_t i = 0; i < c->nclasses; i++) {
      size_t pos = 0;
      bdfi_svg_str have;
      int found = 0;
      while (!found && bdfi_svg_next_field(v, &pos, &have)) {
        found = bdfi_svg_str_cmp(have, c->classes[i]) == 0;
      }
      if (!found) {
        return 0;
      }
    }
  }
  return 1;
}

/* cssRule.matches in css.go: the last part matches the element, and the
 * parts before it ancestors of it, in their order. */
static int bdfi_svg_rule_matches(const bdfi_svg_rule *r, const bdfi_svg_node *n) {
  size_t k = r->nsel - 1; /* the parts before sel[k] are still to be found */
  if (!bdfi_svg_compound_matches(&r->sel[k], n)) {
    return 0;
  }
  for (const bdfi_svg_node *p = n->parent; p && k > 0; p = p->parent) {
    if (bdfi_svg_compound_matches(&r->sel[k - 1], p)) {
      k--;
    }
  }
  return k == 0;
}

/* match in styleSheet.apply: the rules of a key against an element, while
 * the count of them allows. */
static void bdfi_svg_match(bdfi_svg_css *css, const bdfi_svg_group *g, const bdfi_svg_node *n) {
  if (!g) {
    return;
  }
  for (size_t i = 0; i < g->count; i++) {
    css->matches++;
    if (css->matches > css->most) {
      return;
    }
    const bdfi_svg_rule *r = css->keyed[g->first + i].rule;
    if (!bdfi_svg_rule_matches(r, n)) {
      continue;
    }
    if (css->nhits == css->hits_cap) {
      size_t cap = css->hits_cap ? css->hits_cap * 2 : 16;
      const bdfi_svg_rule **p =
          cap > css->hits_cap ? bdfi_realloc_array(css->hits, cap, sizeof *p) : NULL;
      if (!p) {
        css->status = BDF_ERR_NOMEM;
        return;
      }
      css->hits = p;
      css->hits_cap = cap;
    }
    css->hits[css->nhits++] = r;
  }
}

/* The order rules win in: the more specific and the later ones last. */
static int bdfi_svg_hit_cmp(const void *a, const void *b) {
  const bdfi_svg_rule *x = *(const bdfi_svg_rule *const *)a, *y = *(const bdfi_svg_rule *const *)b;
  if (x->spec != y->spec) {
    return x->spec < y->spec ? -1 : 1;
  }
  return x->order < y->order ? -1 : x->order > y->order;
}

/* Node.Sheet of an element from the rules that match it, sorted: the
 * declarations of each put over those of the ones before it. */
static void bdfi_svg_build_sheet(bdfi_svg_css *css, bdfi_svg_node *n) {
  n->has_sheet = 1;
  if (css->nhits == css->nlast &&
      memcmp(css->hits, css->last_hits, css->nhits * sizeof *css->hits) == 0) {
    n->sheet = css->last_sheet;
    n->nsheet = css->last_n;
    return;
  }
  const bdfi_svg_block *first = css->hits[0]->block;
  size_t total = first->n;
  int one = 1;
  for (size_t i = 1; i < css->nhits; i++) {
    const bdfi_svg_block *b = css->hits[i]->block;
    if (b != first) {
      one = 0;
    }
    if (b->n > SIZE_MAX - total) {
      css->status = BDF_ERR_NOMEM;
      return;
    }
    total += b->n;
  }
  if (one) {
    n->sheet = first->ptrs;
    n->nsheet = first->n;
  } else {
    if (total > css->merge_cap) {
      for (int k = 0; k < 2; k++) {
        const bdfi_svg_decl **p = bdfi_realloc_array(css->merge[k], total, sizeof *p);
        if (!p) {
          css->status = BDF_ERR_NOMEM;
          return;
        }
        css->merge[k] = p;
      }
      css->merge_cap = total;
    }
    const bdfi_svg_decl **acc = css->merge[0], **out = css->merge[1];
    size_t na = first->n;
    if (na > 0) {
      memcpy(acc, first->ptrs, na * sizeof *acc);
    }
    for (size_t h = 1; h < css->nhits; h++) {
      const bdfi_svg_block *b = css->hits[h]->block;
      if (b == css->hits[h - 1]->block) {
        continue; /* the same declarations again */
      }
      css->work += (uint64_t)na + b->n;
      if (css->work > BDFI_SVG_MAX_SHEET_WORK) {
        css->status = BDF_ERR_TOO_LARGE;
        return;
      }
      size_t i = 0, j = 0, k = 0;
      while (i < na && j < b->n) {
        int c = bdfi_svg_str_cmp(acc[i]->name, b->ptrs[j]->name);
        if (c < 0) {
          out[k++] = acc[i++];
        } else {
          if (c == 0) {
            i++; /* the later rule wins */
          }
          out[k++] = b->ptrs[j++];
        }
      }
      while (i < na) {
        out[k++] = acc[i++];
      }
      while (j < b->n) {
        out[k++] = b->ptrs[j++];
      }
      const bdfi_svg_decl **t = acc;
      acc = out;
      out = t;
      na = k;
    }
    if (na > BDFI_SVG_MAX_SHEET_DECLS - css->stored) {
      css->status = BDF_ERR_TOO_LARGE;
      return;
    }
    css->stored += na;
    const bdfi_svg_decl **kept = bdfi_svg_pool_array(css->pool, na, sizeof *kept);
    if (!kept) {
      css->status = BDF_ERR_NOMEM;
      return;
    }
    if (na > 0) {
      memcpy(kept, acc, na * sizeof *kept);
    }
    n->sheet = kept;
    n->nsheet = na;
  }
  /* the next element may be matched by the same rules */
  if (css->nhits > css->last_cap) {
    const bdfi_svg_rule **p = bdfi_realloc_array(css->last_hits, css->nhits, sizeof *p);
    if (!p) {
      css->status = BDF_ERR_NOMEM;
      return;
    }
    css->last_hits = p;
    css->last_cap = css->nhits;
  }
  memcpy(css->last_hits, css->hits, css->nhits * sizeof *css->hits);
  css->nlast = css->nhits;
  css->last_sheet = n->sheet;
  css->last_n = n->nsheet;
}

/* styleSheet.apply in css.go, for one element: it is matched against the
 * rules that ask for its id, one of its classes or its name, and those that
 * ask for none. */
static void bdfi_svg_apply_node(bdfi_svg_css *css, bdfi_svg_node *n) {
  bdfi_svg_str v;
  css->nhits = 0;
  css->visit++;
  if (bdfi_svg_attr(n, "id", 2, &v) && v.n != 0) {
    bdfi_svg_match(css, bdfi_svg_find_group(css, BDFI_SVG_BY_ID, v), n);
  }
  if (bdfi_svg_attr(n, "class", 5, &v) && v.n != 0) {
    size_t pos = 0;
    bdfi_svg_str c;
    while (bdfi_svg_next_field(v, &pos, &c)) {
      /* a class the element names twice is matched once */
      bdfi_svg_group *g = bdfi_svg_find_group(css, BDFI_SVG_BY_CLASS, c);
      if (g && g->stamp != css->visit) {
        g->stamp = css->visit;
        bdfi_svg_match(css, g, n);
      }
    }
  }
  bdfi_svg_match(css, bdfi_svg_find_group(css, BDFI_SVG_BY_TAG, n->name), n);
  bdfi_svg_match(css, bdfi_svg_find_group(css, BDFI_SVG_OTHERS, bdfi_svg_str_of("", 0)), n);
  if (css->status != BDF_OK || css->nhits == 0) {
    return;
  }
  if (bdfi_svg_sort(css->hits, css->nhits, sizeof *css->hits, bdfi_svg_hit_cmp) != 0) {
    css->status = BDF_ERR_NOMEM;
    return;
  }
  bdfi_svg_build_sheet(css, n);
}

static bdfi_svg_node *bdfi_svg_first_element(bdfi_svg_node *n) {
  while (n && n->is_text) {
    n = n->next;
  }
  return n;
}

bdf_status bdfi_svg_apply_css(bdfi_svg_pool *pool, bdfi_svg_str src, bdfi_svg_node *root,
                              int64_t most, int *over, const char **msg) {
  bdfi_svg_css css;
  memset(&css, 0, sizeof css);
  bdfi_arena_init(&css.tmp);
  css.pool = pool;
  css.most = most;
  css.status = BDF_OK;
  *over = 0;
  *msg = "";
  /* the sheet without the comments, in one pass; a comment that does not
   * end takes the rest */
  char *text = bdfi_svg_pool_bytes(pool, src.n);
  if (!text) {
    return BDF_ERR_NOMEM;
  }
  size_t n = 0, pos = 0;
  while (pos < src.n) {
    const char *slash = memchr(src.p + pos, '/', src.n - pos);
    while (slash && !((size_t)(slash - src.p) + 1 < src.n && slash[1] == '*')) {
      size_t from = (size_t)(slash - src.p) + 1;
      slash = from < src.n ? memchr(src.p + from, '/', src.n - from) : NULL;
    }
    size_t open = slash ? (size_t)(slash - src.p) : src.n;
    memcpy(text + n, src.p + pos, open - pos);
    n += open - pos;
    if (!slash) {
      break;
    }
    pos = src.n; /* unless the comment ends */
    for (size_t j = open + 2; j + 1 < src.n; j++) {
      const char *star = memchr(src.p + j, '*', src.n - 1 - j);
      if (!star) {
        break;
      }
      j = (size_t)(star - src.p);
      if (src.p[j + 1] == '/') {
        pos = j + 2;
        break;
      }
    }
  }
  bdf_status st = bdfi_svg_parse_rules(&css, bdfi_svg_str_of(text, n));
  if (st == BDF_ERR_FORMAT) {
    *msg = bdfi_svg_msg_selector();
  }
  if (st == BDF_OK && css.nrules > 0) {
    if (bdfi_svg_index_rules(&css) != 0) {
      st = BDF_ERR_NOMEM;
    }
    /* styleSheet.apply walks the elements in the order of the document,
     * while the count of the rules matched allows */
    bdfi_svg_node *node = st == BDF_OK ? root : NULL;
    while (node) {
      bdfi_svg_apply_node(&css, node);
      if (css.status != BDF_OK || css.matches > css.most) {
        break;
      }
      bdfi_svg_node *next = bdfi_svg_first_element(node->first_child);
      while (!next && node != root) {
        next = bdfi_svg_first_element(node->next);
        if (!next) {
          node = node->parent;
        }
      }
      node = next;
    }
    if (st == BDF_OK) {
      st = css.status;
    }
    if (st == BDF_ERR_TOO_LARGE) {
      *msg = bdfi_svg_msg_sheets();
    }
    *over = css.matches > css.most;
  }
  free(css.rules);
  free(css.keyed);
  free(css.groups);
  free(css.hits);
  free(css.last_hits);
  free(css.merge[0]);
  free(css.merge[1]);
  bdfi_arena_free(&css.tmp);
  return st;
}
