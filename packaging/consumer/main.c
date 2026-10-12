/* What a consumer of the installed library does first: it says its version,
 * and that what is no document is none. Built by the checks of the packages
 * (CMake package, pkg-config, Conan, vcpkg, Meson). */
#include <stdio.h>
#include <string.h>

#include <bdf/bdf.h>

int main(void) {
  static const unsigned char junk[] = "not a document";
  bdf_doc *doc = NULL;
  bdf_error err;
  if (bdf_doc_open_memory(junk, sizeof junk - 1, &doc, &err) == BDF_OK) {
    fprintf(stderr, "junk opened as a document\n");
    return 1;
  }
  if (strcmp(bdf_version(), BDF_VERSION_STRING) != 0) {
    fprintf(stderr, "the library is %s, its header %s\n", bdf_version(), BDF_VERSION_STRING);
    return 1;
  }
  printf("libbdf %s\n", bdf_version());
  return 0;
}
