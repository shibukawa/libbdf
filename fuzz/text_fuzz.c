/* A libFuzzer target over the text layer: the index parts, link targets, and
 * the walk of an object that resolves every part name to itself. */
#include <stddef.h>
#include <stdint.h>

#include "bdf/bdf.h"

static const bdf_object *itself(void *ctx, const uint8_t hash[16]) {
  (void)hash;
  return ctx;
}

/* uses counts the USE instructions of an object. */
static size_t uses(const bdf_object *o) {
  bdf_instr *it = bdf_instr_new(o);
  bdf_error err;
  size_t n = 0;
  while (it && bdf_instr_next(it, &err) > 0) {
    uint8_t op = bdf_instr_op(it);
    n += op == BDF_OP_USE || op == BDF_OP_USE_AT;
  }
  bdf_instr_free(it);
  return n;
}

static void touch_text(bdf_text *t) {
  volatile float sink = 0;
  for (size_t i = 0; i < bdf_text_run_count(t); i++) {
    bdf_text_run r;
    bdf_text_run_at(t, i, &r);
    sink += r.x + r.y + (float)r.text_len + (float)(r.has_font ? r.font.weight : 0);
    if (r.text_len > 0) {
      sink += (float)r.text[r.text_len - 1];
    }
  }
  for (size_t i = 0; i < bdf_text_link_count(t); i++) {
    bdf_text_link l;
    bdf_link_target target;
    char id[64];
    bdf_text_link_at(t, i, &l);
    bdf_link_parse(l.url, l.url_len, &target);
    sink += l.w + (float)bdf_link_view_id(&target, id, sizeof id);
  }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  bdf_error err;
  bdf_text_index *idx;
  if (bdf_text_index_decode(data, size, &idx, &err) == BDF_OK) {
    char *plain;
    size_t n;
    if (bdf_text_index_plain(idx, &plain, &n, &err) == BDF_OK) {
      bdf_free(plain);
    }
    bdf_text_index_free(idx);
  }
  bdf_cues *cues;
  if (bdf_cues_decode(data, size, &cues, &err) == BDF_OK) {
    bdf_cue q;
    bdf_cue_system s;
    for (size_t i = 0; i < bdf_cues_count(cues); i++) {
      bdf_cues_cue(cues, i, &q);
      bdf_cues_system(cues, q.system, &s);
    }
    bdf_cues_free(cues);
  }
  bdf_link_target target;
  char id[32];
  bdf_link_parse((const char *)data, size, &target);
  bdf_link_view_id(&target, id, sizeof id);

  bdf_object *o;
  if (bdf_object_decode(data, size, &o, &err) == BDF_OK) {
    /* an object that draws itself more than once multiplies its walks up to
     * the limit of reused instructions, which takes seconds: it is walked
     * without what it draws */
    bdf_text *t;
    if (bdf_text_extract(o, uses(o) <= 1 ? itself : NULL, o, &t, &err) == BDF_OK) {
      touch_text(t);
      bdf_text_free(t);
    }
    bdf_object_free(o);
  }
  return 0;
}
