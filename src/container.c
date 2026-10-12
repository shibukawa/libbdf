/* The single-file container header (spec §3.3), with the checks of
 * OpenSingle in reader.go of the reference implementation, and the hash
 * helpers. */
#include <string.h>

#include "bdf_internal.h"

bdf_status bdf_header_parse(const uint8_t *data, size_t len, uint64_t file_size, bdf_header *out,
                            bdf_error *err) {
  bdfi_error_clear(err);
  if (!out || (!data && len > 0)) {
    bdfi_error_set(err, BDF_ERR_ARG, 0, "bad argument");
    return BDF_ERR_ARG;
  }
  memset(out, 0, sizeof *out);
  if (len < BDF_HEADER_SIZE) {
    bdfi_error_set(err, BDF_ERR_FORMAT, len, "short header");
    return BDF_ERR_FORMAT;
  }
  bdfi_reader rd, *r = &rd;
  bdfi_rd_init(r, data, BDF_HEADER_SIZE);
  if (memcmp(bdfi_rd_bytes(r, 4), "bdf\0", 4) != 0) {
    bdfi_error_set(err, BDF_ERR_FORMAT, 0, "bad magic");
    return BDF_ERR_FORMAT;
  }
  out->version = bdfi_rd_u16(r);
  if (out->version != BDF_FORMAT_VERSION) {
    bdfi_error_set(err, BDF_ERR_FORMAT, 4, "unsupported format version");
    return BDF_ERR_FORMAT;
  }
  out->flags = bdfi_rd_u16(r);
  if ((out->flags & ~BDF_FLAG_ENCRYPTED) != 0) {
    bdfi_error_set(err, BDF_ERR_FORMAT, 6, "unsupported header flags");
    return BDF_ERR_FORMAT;
  }
  out->manifest_off = bdfi_rd_u64(r);
  out->manifest_len = bdfi_rd_u64(r);
  out->manifest_enc = bdfi_rd_u8(r);
  if (out->manifest_enc > 1) {
    bdfi_error_set(err, BDF_ERR_FORMAT, 24, "unknown manifest encoding");
    return BDF_ERR_FORMAT;
  }
  for (size_t i = 25; i < BDF_HEADER_SIZE; i++) {
    if (data[i] != 0) {
      bdfi_error_set(err, BDF_ERR_FORMAT, i, "non-zero reserved header bytes");
      return BDF_ERR_FORMAT;
    }
  }
  if (out->manifest_off > file_size || out->manifest_len > file_size - out->manifest_off) {
    bdfi_error_set(err, BDF_ERR_FORMAT, 8, "manifest out of range");
    return BDF_ERR_FORMAT;
  }
  if (out->manifest_len > BDF_MAX_MANIFEST_SIZE) {
    bdfi_error_set(err, BDF_ERR_FORMAT, 16, "manifest too large");
    return BDF_ERR_FORMAT;
  }
  return BDF_OK;
}

static int bdfi_hex_value(char c) {
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

bdf_status bdf_hash_parse(const char *s, size_t len, uint8_t out[16]) {
  if (!s || !out || len != 32) {
    return BDF_ERR_FORMAT;
  }
  for (size_t i = 0; i < 16; i++) {
    int hi = bdfi_hex_value(s[2 * i]), lo = bdfi_hex_value(s[2 * i + 1]);
    if (hi < 0 || lo < 0) {
      return BDF_ERR_FORMAT;
    }
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return BDF_OK;
}

void bdf_hash_format(const uint8_t hash[16], char out[33]) {
  static const char digits[] = "0123456789abcdef";
  if (!hash || !out) {
    return;
  }
  for (size_t i = 0; i < 16; i++) {
    out[2 * i] = digits[hash[i] >> 4];
    out[2 * i + 1] = digits[hash[i] & 15];
  }
  out[32] = '\0';
}

int bdf_tiles_index_find(const uint8_t *idx, size_t len, uint32_t tx, uint32_t ty,
                         uint8_t out[16]) {
  if (!idx || !out || len % 24 != 0) {
    return 0;
  }
  size_t lo = 0, hi = len / 24;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    const uint8_t *rec = idx + mid * 24;
    uint32_t rx = (uint32_t)rec[0] | ((uint32_t)rec[1] << 8) | ((uint32_t)rec[2] << 16) |
                  ((uint32_t)rec[3] << 24);
    uint32_t ry = (uint32_t)rec[4] | ((uint32_t)rec[5] << 8) | ((uint32_t)rec[6] << 16) |
                  ((uint32_t)rec[7] << 24);
    if (ry < ty || (ry == ty && rx < tx)) {
      lo = mid + 1;
    } else if (ry == ty && rx == tx) {
      memcpy(out, rec + 8, 16);
      return 1;
    } else {
      hi = mid;
    }
  }
  return 0;
}
