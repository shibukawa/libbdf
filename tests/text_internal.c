/* Tests of the text layer's internals: the set of part names, and the limit
 * of reused instructions, lowered so that passing it takes no time. */
#include "testutil.h"

#include "bdf_internal.h"

static void test_hash_set(void) {
  bdfi_hash_set s;
  memset(&s, 0, sizeof s);
  uint8_t key[16];

  /* scattered names */
  uint64_t x = 0x9E3779B97F4A7C15ULL;
  for (int i = 0; i < 5000; i++) {
    for (int k = 0; k < 16; k++) {
      x ^= x << 13;
      x ^= x >> 7;
      x ^= x << 17;
      key[k] = (uint8_t)x;
    }
    CHECK(bdfi_hash_set_add(&s, key) == 0);
    CHECK(bdfi_hash_set_add(&s, key) == 1);
  }
  /* names made to share: one bit set each, a common prefix of 15 bytes, and
   * the two ends */
  memset(key, 0, sizeof key);
  CHECK(bdfi_hash_set_add(&s, key) == 0);
  for (int bit = 0; bit < 128; bit++) {
    memset(key, 0, sizeof key);
    key[bit / 8] = (uint8_t)(0x80 >> (bit % 8));
    CHECK(bdfi_hash_set_add(&s, key) == 0);
  }
  for (int bit = 127; bit >= 0; bit--) {
    memset(key, 0, sizeof key);
    key[bit / 8] = (uint8_t)(0x80 >> (bit % 8));
    CHECK(bdfi_hash_set_add(&s, key) == 1);
  }
  memset(key, 0xAB, sizeof key);
  for (int last = 0; last < 256; last++) {
    key[15] = (uint8_t)last;
    CHECK(bdfi_hash_set_add(&s, key) == 0);
  }
  for (int last = 255; last >= 0; last--) {
    key[15] = (uint8_t)last;
    CHECK(bdfi_hash_set_add(&s, key) == 1);
  }
  memset(key, 0xFF, sizeof key);
  CHECK(bdfi_hash_set_add(&s, key) == 0 && bdfi_hash_set_add(&s, key) == 1);
  memset(key, 0, sizeof key);
  CHECK(bdfi_hash_set_add(&s, key) == 1);
  CHECK(s.nkeys == 5000 + 1 + 128 + 256 + 1 && s.nnodes == s.nkeys - 1);
  /* no path is longer than the names have bits */
  for (size_t i = 0; i < s.nnodes; i++) {
    CHECK(s.nodes[i].bit < 128);
    for (int side = 0; side < 2; side++) {
      int32_t c = s.nodes[i].child[side];
      CHECK(c < 0 || s.nodes[c].bit > s.nodes[i].bit);
    }
  }
  bdfi_hash_set_free(&s);
  CHECK(s.nkeys == 0 && s.keys == NULL);
}

typedef struct objs {
  bdf_object **o;
  size_t n;
} objs;

static const bdf_object *by_id(void *ctx, const uint8_t hash[16]) {
  const objs *t = ctx;
  size_t id = ((size_t)hash[0] << 24) | ((size_t)hash[1] << 16) | ((size_t)hash[2] << 8) | hash[3];
  return id < t->n ? t->o[id] : NULL;
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

static void test_reuse_limit(void) {
  bdf_text *t = NULL;
  bdf_error err;
  bdfi_use_limits limits;

  /* what is read from objects that were walked before is counted: of three
   * levels that each draw the next twice, 15 instructions */
  objs small = nested(3, 2);
  bdfi_use_limits_init(&limits);
  CHECK(bdfi_text_extract(small.o[0], by_id, &small, &limits, 0, &t, &err) == BDF_OK);
  CHECK(bdf_text_run_count(t) == 8 && limits.reused == 15 && limits.seen.nkeys == 3);
  bdf_text_free(t);
  /* the limits go on counting for the next object of the view: all of what
   * it draws was walked before */
  CHECK(bdfi_text_extract(small.o[0], by_id, &small, &limits, 0, &t, &err) == BDF_OK);
  CHECK(bdf_text_run_count(t) == 8 && limits.reused == 15 + 20 && limits.seen.nkeys == 3);
  bdf_text_free(t);
  bdfi_use_limits_free(&limits);

  /* TestExtractTextNesting, fan out: 4^30 texts, were they all drawn */
  objs wide = nested(30, 4);
  bdfi_use_limits_init(&limits);
  limits.max_reused = 1 << 12;
  CHECK(bdfi_text_extract(wide.o[0], by_id, &wide, &limits, 0, &t, &err) == BDF_ERR_FORMAT && t == NULL);
  CHECK_MSG(err, "objects are drawn too many times");
  CHECK(limits.reused == (1 << 12) + 1);
  /* a limit that was passed holds for whatever is walked with it next */
  CHECK(bdfi_text_extract(small.o[3], by_id, &small, &limits, 0, &t, &err) == BDF_ERR_FORMAT && t == NULL);
  CHECK_MSG(err, "objects are drawn too many times");
  bdfi_use_limits_free(&limits);
  /* and not for a walk of its own */
  CHECK(bdf_text_extract(small.o[3], by_id, &small, &t, &err) == BDF_OK && bdf_text_run_count(t) == 1);
  bdf_text_free(t);
  free_objs(&wide);
  free_objs(&small);
}

int main(void) {
  test_hash_set();
  test_reuse_limit();
  return finish();
}
