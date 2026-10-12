# Brotli decoder (vendored)

The decoder of [google/brotli](https://github.com/google/brotli) 1.1.0, as libbdf needs it to open WOFF2 fonts: `c/common`, `c/dec` and the public headers `decode.h`, `port.h`, `types.h` and `shared_dictionary.h` of the release archive `v1.1.0.tar.gz` (SHA-256 `e720a6ca29428b803f4ad165371771f5398faba397edf6778837a18599ea13ff`). The encoder is not included. The files are unmodified; the license is in LICENSE (MIT).

CMake builds them into libbdf when `BDF_WITH_BROTLI` is ON (the default) and does not export their symbols. A build with the option OFF, and the single-header form without `BDF_HAVE_BROTLI`, refuse WOFF2 fonts with `BDF_ERR_UNSUPPORTED`.
