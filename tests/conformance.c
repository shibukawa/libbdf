/*
 * Runs a conformance suite written by `bdf conformance` of the reference
 * implementation: suite.json names cases, their inputs and what a reader
 * must do with them (open or refuse a document, decode an object, read
 * parts). It is built from the library's sources so that it can use the
 * internal JSON reader; the suite's format is described in docs/design.md.
 *
 *   test_conformance <suite directory>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bdf_internal.h"

static int failures, checked;

static uint8_t *read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    return NULL;
  }
  size_t cap = 1 << 16, n = 0;
  uint8_t *buf = malloc(cap);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  for (;;) {
    n += fread(buf + n, 1, cap - n, f);
    if (n < cap) {
      break;
    }
    uint8_t *grown = realloc(buf, cap * 2);
    if (!grown) {
      free(buf);
      fclose(f);
      return NULL;
    }
    buf = grown;
    cap *= 2;
  }
  fclose(f);
  *len = n;
  return buf;
}

static void fail(const char *id, const char *what, const char *want, const char *got) {
  failures++;
  fprintf(stderr, "%s: %s: want %s, got %s\n", id, what, want, got);
}

/* ---- JSON helpers ------------------------------------------------------- */

static const char *str_member(const bdfi_json *j, const bdfi_json_node *obj, const char *key,
                              const char *dflt) {
  const bdfi_json_node *n = bdfi_json_member(j, obj, key);
  if (!n || n->kind != BDFI_JSON_STRING) {
    return dflt;
  }
  size_t len;
  return bdfi_json_text(j, n, 0, &len); /* NUL-terminated: the buffer is ours */
}

static int int_member(const bdfi_json *j, const bdfi_json_node *obj, const char *key,
                      int64_t *out) {
  const bdfi_json_node *n = bdfi_json_member(j, obj, key);
  return n && bdfi_json_int64(j, n, out) == 0;
}

static int bool_member(const bdfi_json *j, const bdfi_json_node *obj, const char *key) {
  const bdfi_json_node *n = bdfi_json_member(j, obj, key);
  return n && n->kind == BDFI_JSON_TRUE;
}

/* ---- Cases -------------------------------------------------------------- */

static void check_single(const bdfi_json *j, const char *id, const bdfi_json_node *expect,
                         const uint8_t *data, size_t len) {
  bdf_doc *doc = NULL;
  bdf_error err;
  bdf_status st = bdf_doc_open_memory(data, len, &doc, &err);
  const char *want = str_member(j, expect, "open", "ok");
  const char *got = st == BDF_OK ? "ok" : "error";
  checked++;
  if (strcmp(want, got) != 0) {
    char buf[256];
    snprintf(buf, sizeof buf, "%s (%s)", got, st == BDF_OK ? "" : err.message);
    fail(id, "open", want, buf);
    bdf_doc_free(doc);
    return;
  }
  if (st != BDF_OK) {
    return;
  }
  if (bool_member(j, expect, "locked") != (bdf_doc_locked(doc) != 0)) {
    fail(id, "locked", bool_member(j, expect, "locked") ? "true" : "false",
         bdf_doc_locked(doc) ? "true" : "false");
  }
  const bdfi_json_node *views = bdfi_json_member(j, expect, "views");
  if (views && views->kind == BDFI_JSON_ARRAY) {
    char w[32], g[32];
    snprintf(w, sizeof w, "%u", (unsigned)views->count);
    snprintf(g, sizeof g, "%u", (unsigned)bdf_doc_view_count(doc));
    if (views->count != bdf_doc_view_count(doc)) {
      fail(id, "view count", w, g);
    } else {
      size_t i = 0;
      for (const bdfi_json_node *v = bdfi_json_first(j, views); v; v = bdfi_json_next(j, v), i++) {
        bdf_view view;
        bdf_doc_view(doc, i, &view);
        const char *vid = str_member(j, v, "id", "");
        const char *kind = str_member(j, v, "kind", "");
        int64_t pages = 0;
        int_member(j, v, "pages", &pages);
        if (strcmp(vid, view.id) != 0) {
          fail(id, "view id", vid, view.id);
        }
        if (strcmp(kind, view.kind) != 0) {
          fail(id, "view kind", kind, view.kind);
        }
        if ((size_t)pages != view.npages) {
          snprintf(w, sizeof w, "%lld", (long long)pages);
          snprintf(g, sizeof g, "%u", (unsigned)view.npages);
          fail(id, "page count", w, g);
        }
      }
    }
  }
  int64_t nparts;
  if (int_member(j, expect, "parts", &nparts) && (size_t)nparts != bdf_doc_part_count(doc)) {
    char w[32], g[32];
    snprintf(w, sizeof w, "%lld", (long long)nparts);
    snprintf(g, sizeof g, "%u", (unsigned)bdf_doc_part_count(doc));
    fail(id, "part count", w, g);
  }
  const bdfi_json_node *reads = bdfi_json_member(j, expect, "reads");
  if (reads && reads->kind == BDFI_JSON_ARRAY) {
    for (const bdfi_json_node *rd = bdfi_json_first(j, reads); rd; rd = bdfi_json_next(j, rd)) {
      const char *h = str_member(j, rd, "h", "");
      uint8_t hash[16];
      if (bdf_hash_parse(h, strlen(h), hash) != BDF_OK) {
        fail(id, "read", h, "not a part name");
        continue;
      }
      uint8_t *out;
      size_t n;
      bdf_status rs = bdf_doc_part(doc, hash, &out, &n, &err);
      const char *rwant = str_member(j, rd, "read", "ok");
      const char *rgot = rs == BDF_OK ? "ok" : "error";
      checked++;
      if (strcmp(rwant, rgot) != 0) {
        char what[64];
        snprintf(what, sizeof what, "read %.8s", h);
        fail(id, what, rwant, rs == BDF_OK ? "ok" : err.message);
      }
      int64_t size;
      if (rs == BDF_OK && int_member(j, rd, "size", &size) && (size_t)size != n) {
        char w[32], g[32], what[64];
        snprintf(w, sizeof w, "%lld", (long long)size);
        snprintf(g, sizeof g, "%u", (unsigned)n);
        snprintf(what, sizeof what, "size of %.8s", h);
        fail(id, what, w, g);
      }
      if (rs == BDF_OK) {
        bdf_free(out);
      }
    }
  }
  if (bool_member(j, expect, "allParts")) {
    for (size_t i = 0; i < bdf_doc_part_count(doc); i++) {
      bdf_part_entry e;
      uint8_t *out;
      size_t n;
      bdf_doc_part_entry(doc, i, &e);
      checked++;
      if (bdf_doc_part(doc, e.hash, &out, &n, &err) != BDF_OK) {
        char hex[33];
        bdf_hash_format(e.hash, hex);
        fail(id, hex, "ok", err.message);
      } else {
        bdf_free(out);
      }
    }
  }
  bdf_doc_free(doc);
}

static void check_object(const bdfi_json *j, const char *id, const bdfi_json_node *expect,
                         const uint8_t *data, size_t len) {
  bdf_object *o = NULL;
  bdf_error err;
  bdf_status st = bdf_object_decode(data, len, &o, &err);
  const char *want = str_member(j, expect, "decode", "ok");
  checked++;
  if (strcmp(want, st == BDF_OK ? "ok" : "error") != 0) {
    fail(id, "decode", want, st == BDF_OK ? "ok" : err.message);
    bdf_object_free(o);
    return;
  }
  if (st != BDF_OK) {
    return;
  }
  bdf_instr *it = bdf_instr_new(o);
  int64_t n = 0, rc;
  while ((rc = bdf_instr_next(it, &err)) > 0) {
    n++;
  }
  const char *wwalk = str_member(j, expect, "walk", "ok");
  const char *gwalk = rc < 0 ? "error" : "ok";
  if (strcmp(wwalk, gwalk) != 0) {
    fail(id, "walk", wwalk, rc < 0 ? err.message : "ok");
  }
  int64_t want_n;
  if (int_member(j, expect, "instructions", &want_n) && want_n != n) {
    char w[32], g[32];
    snprintf(w, sizeof w, "%lld", (long long)want_n);
    snprintf(g, sizeof g, "%lld", (long long)n);
    fail(id, "instructions", w, g);
  }
  bdf_instr_free(it);
  bdf_object_free(o);
}

static void check_paths(const bdfi_json *j, const char *id, const bdfi_json_node *expect,
                        const uint8_t *data, size_t len) {
  bdf_paths *ps = NULL;
  bdf_error err;
  bdf_status st = bdf_paths_decode(data, len, &ps, &err);
  const char *want = str_member(j, expect, "decode", "ok");
  checked++;
  if (strcmp(want, st == BDF_OK ? "ok" : "error") != 0) {
    fail(id, "decode", want, st == BDF_OK ? "ok" : err.message);
  } else if (st == BDF_OK) {
    int64_t want_n;
    if (int_member(j, expect, "paths", &want_n) && (size_t)want_n != bdf_paths_count(ps)) {
      char w[32], g[32];
      snprintf(w, sizeof w, "%lld", (long long)want_n);
      snprintf(g, sizeof g, "%u", (unsigned)bdf_paths_count(ps));
      fail(id, "paths", w, g);
    }
  }
  bdf_paths_free(ps);
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: test_conformance <suite directory>\n");
    return 2;
  }
  const char *dir = argv[1];
  char path[4096];
  snprintf(path, sizeof path, "%s/suite.json", dir);
  size_t len;
  uint8_t *text = read_file(path, &len);
  if (!text) {
    fprintf(stderr, "cannot read %s\n", path);
    return 2;
  }
  bdfi_json j;
  bdf_error err;
  if (bdfi_json_parse(text, len, &j, &err) != BDF_OK) {
    fprintf(stderr, "%s: %s at %zu\n", path, err.message, err.offset);
    free(text);
    return 2;
  }
  free(text);
  const bdfi_json_node *root = bdfi_json_root(&j);
  const bdfi_json_node *cases = bdfi_json_member(&j, root, "cases");
  if (!cases || cases->kind != BDFI_JSON_ARRAY) {
    fprintf(stderr, "%s: no cases\n", path);
    bdfi_json_free(&j);
    return 2;
  }
  printf("suite %s, %u cases\n", str_member(&j, root, "format", "?"), (unsigned)cases->count);
  for (const bdfi_json_node *c = bdfi_json_first(&j, cases); c; c = bdfi_json_next(&j, c)) {
    const char *id = str_member(&j, c, "id", "?");
    const char *kind = str_member(&j, c, "kind", "");
    const char *input = str_member(&j, c, "input", "");
    const bdfi_json_node *expect = bdfi_json_member(&j, c, "expect");
    snprintf(path, sizeof path, "%s/%s", dir, input);
    size_t n;
    uint8_t *data = read_file(path, &n);
    if (!data) {
      fail(id, "input", input, "unreadable");
      continue;
    }
    if (strcmp(kind, "single") == 0) {
      check_single(&j, id, expect, data, n);
    } else if (strcmp(kind, "object") == 0) {
      check_object(&j, id, expect, data, n);
    } else if (strcmp(kind, "paths") == 0) {
      check_paths(&j, id, expect, data, n);
    } else {
      printf("%s: kind %s not known, skipped\n", id, kind);
    }
    free(data);
  }
  bdfi_json_free(&j);
  printf("%d checks, %d failed\n", checked, failures);
  return failures ? 1 : 0;
}
