# libFuzzer harnesses. Build with clang and -DBDF_BUILD_FUZZ=ON (and usually
# -DBDF_SANITIZE=address,undefined); run on demand, not in CI:
#
#   ./build/fuzz/doc_fuzz fuzz/corpus/doc -max_total_time=600
#
# fuzz/corpus/<target>/ keeps the inputs a fuzzer found interesting (local
# only); what it finds goes to fuzz/regress/<target>/, which the tests replay.
foreach(target object inflate manifest doc woff2 text unlock search render)
  add_executable(${target}_fuzz ${target}_fuzz.c)
  target_link_libraries(${target}_fuzz PRIVATE bdf::bdf)
  target_compile_options(${target}_fuzz PRIVATE -fsanitize=fuzzer)
  target_link_options(${target}_fuzz PRIVATE -fsanitize=fuzzer)
endforeach()
