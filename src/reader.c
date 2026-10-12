#include <stdlib.h>
#include <string.h>

#include "bdf_internal.h"

/* ---- Errors ------------------------------------------------------------- */

void bdfi_error_clear(bdf_error *err) {
  if (err) {
    err->status = BDF_OK;
    err->offset = 0;
    err->message = "";
  }
}

void bdfi_error_set(bdf_error *err, bdf_status status, size_t off, const char *msg) {
  if (err) {
    err->status = status;
    err->offset = off;
    err->message = msg;
  }
}

void bdfi_error_from_reader(bdf_error *err, const bdfi_reader *r) {
  bdfi_error_set(err, BDF_ERR_FORMAT, r->err_off, r->err_msg);
}

/* ---- Reader ------------------------------------------------------------- */

void bdfi_rd_init(bdfi_reader *r, const uint8_t *b, size_t len) {
  r->b = b;
  r->len = len;
  r->pos = 0;
  r->failed = 0;
  r->err_off = 0;
  r->err_msg = "";
}

void bdfi_rd_fail_at(bdfi_reader *r, size_t off, const char *msg) {
  if (!r->failed) {
    r->failed = 1;
    r->err_off = off;
    r->err_msg = msg;
  }
}

void bdfi_rd_fail(bdfi_reader *r, const char *msg) { bdfi_rd_fail_at(r, r->pos, msg); }

int bdfi_rd_need(bdfi_reader *r, size_t n) {
  if (r->failed) {
    return 0;
  }
  if (n > r->len - r->pos) {
    bdfi_rd_fail(r, "unexpected end of data");
    return 0;
  }
  return 1;
}

size_t bdfi_rd_count(bdfi_reader *r, size_t size, const char *msg) {
  uint64_t n = bdfi_rd_varuint(r);
  if (n > (uint64_t)((r->len - r->pos) / size)) {
    bdfi_rd_fail(r, msg);
    return 0;
  }
  return (size_t)n;
}

int bdfi_rd_eof(const bdfi_reader *r) { return r->failed || r->pos >= r->len; }

uint8_t bdfi_rd_u8(bdfi_reader *r) {
  if (!bdfi_rd_need(r, 1)) {
    return 0;
  }
  return r->b[r->pos++];
}

uint16_t bdfi_rd_u16(bdfi_reader *r) {
  if (!bdfi_rd_need(r, 2)) {
    return 0;
  }
  const uint8_t *p = r->b + r->pos;
  r->pos += 2;
  return (uint16_t)(p[0] | (p[1] << 8));
}

uint32_t bdfi_rd_u32(bdfi_reader *r) {
  if (!bdfi_rd_need(r, 4)) {
    return 0;
  }
  const uint8_t *p = r->b + r->pos;
  r->pos += 4;
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint64_t bdfi_rd_u64(bdfi_reader *r) {
  if (!bdfi_rd_need(r, 8)) {
    return 0;
  }
  uint64_t v = 0;
  for (int i = 7; i >= 0; i--) {
    v = (v << 8) | r->b[r->pos + (size_t)i];
  }
  r->pos += 8;
  return v;
}

float bdfi_rd_f32(bdfi_reader *r) {
  uint32_t u = bdfi_rd_u32(r);
  float f;
  memcpy(&f, &u, sizeof f);
  return f;
}

uint64_t bdfi_rd_varuint(bdfi_reader *r) {
  if (r->failed) {
    return 0;
  }
  uint64_t x = 0;
  unsigned s = 0;
  for (size_t i = 0;; i++) {
    if (i == 10 || r->pos + i >= r->len) {
      /* an overflow, or the data ends before the last byte */
      bdfi_rd_fail(r, "bad varuint");
      return 0;
    }
    uint8_t b = r->b[r->pos + i];
    if (b < 0x80) {
      if (i == 9 && b > 1) {
        bdfi_rd_fail(r, "bad varuint");
        return 0;
      }
      r->pos += i + 1;
      return x | ((uint64_t)b << s);
    }
    x |= (uint64_t)(b & 0x7f) << s;
    s += 7;
  }
}

const uint8_t *bdfi_rd_bytes(bdfi_reader *r, size_t n) {
  if (!bdfi_rd_need(r, n)) {
    return NULL;
  }
  const uint8_t *p = r->b + r->pos;
  r->pos += n;
  return p;
}

/* ---- Memory ------------------------------------------------------------- */

void *bdfi_alloc_items(size_t n, size_t size, int *oom) {
  if (n == 0) {
    return NULL;
  }
  if (n > SIZE_MAX / size) {
    *oom = 1;
    return NULL;
  }
  void *p = calloc(n, size);
  if (!p) {
    *oom = 1;
  }
  return p;
}

struct bdfi_arena_chunk {
  bdfi_arena_chunk *next;
  size_t used, cap;
  /* the memory follows */
};

#define BDFI_ARENA_CHUNK (64 * 1024)
#define BDFI_ARENA_ALIGN 16

void bdfi_arena_init(bdfi_arena *a) { a->chunks = NULL; }

void *bdfi_arena_alloc(bdfi_arena *a, size_t n) {
  size_t need = (n + BDFI_ARENA_ALIGN - 1) & ~(size_t)(BDFI_ARENA_ALIGN - 1);
  if (need < n) {
    return NULL; /* overflow */
  }
  bdfi_arena_chunk *c = a->chunks;
  if (!c || c->cap - c->used < need) {
    size_t cap = need > BDFI_ARENA_CHUNK ? need : BDFI_ARENA_CHUNK;
    size_t header = (sizeof *c + BDFI_ARENA_ALIGN - 1) & ~(size_t)(BDFI_ARENA_ALIGN - 1);
    if (cap > SIZE_MAX - header) {
      return NULL;
    }
    c = calloc(1, header + cap);
    if (!c) {
      return NULL;
    }
    c->next = a->chunks;
    c->cap = cap;
    c->used = 0;
    a->chunks = c;
  }
  size_t header = (sizeof *c + BDFI_ARENA_ALIGN - 1) & ~(size_t)(BDFI_ARENA_ALIGN - 1);
  void *p = (char *)c + header + c->used;
  c->used += need;
  return p;
}

char *bdfi_arena_strdup(bdfi_arena *a, const char *s, size_t n) {
  char *p = bdfi_arena_alloc(a, n + 1);
  if (!p) {
    return NULL;
  }
  if (n > 0) {
    memcpy(p, s, n);
  }
  p[n] = '\0';
  return p;
}

void bdfi_arena_free(bdfi_arena *a) {
  bdfi_arena_chunk *c = a->chunks;
  while (c) {
    bdfi_arena_chunk *next = c->next;
    free(c);
    c = next;
  }
  a->chunks = NULL;
}

/* ---- Free --------------------------------------------------------------- */

void bdf_free(void *p) { free(p); }
