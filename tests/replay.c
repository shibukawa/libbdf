/*
 * Replays the inputs the fuzzers found (fuzz/regress/<target>/, committed as
 * regression tests) through the fuzz target as a plain program, so that the
 * ordinary test build and the sanitizer job run them without libFuzzer.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static int replay(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "%s: cannot open\n", path);
    return 1;
  }
  size_t cap = 1 << 16, n = 0;
  uint8_t *buf = malloc(cap);
  if (!buf) {
    fclose(f);
    return 1;
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
      return 1;
    }
    buf = grown;
    cap *= 2;
  }
  fclose(f);
  LLVMFuzzerTestOneInput(buf, n);
  free(buf);
  printf("%s: %zu bytes ok\n", path, n);
  return 0;
}

int main(int argc, char **argv) {
  int rc = 0;
  for (int i = 1; i < argc; i++) {
    rc |= replay(argv[i]);
  }
  if (argc == 1) {
    printf("no regression inputs\n");
  }
  return rc;
}
