set(BDF_TESTS object header inflate manifest doc font text search unlock render)
set(BDF_FUZZ_TARGETS object inflate manifest doc woff2 text unlock search render image)

if(MSVC)
  set(BDF_TEST_WARNINGS /W4)
else()
  set(BDF_TEST_WARNINGS -Wall -Wextra)
endif()

# The unit tests against the library as built.
foreach(name ${BDF_TESTS})
  add_executable(test_${name} test_${name}.c)
  target_link_libraries(test_${name} PRIVATE bdf::bdf)
  target_compile_options(test_${name} PRIVATE ${BDF_TEST_WARNINGS})
  add_test(NAME ${name} COMMAND test_${name})
endforeach()

# The inputs the fuzzers found, replayed through each fuzz target as a plain
# program: fuzz/regress/<target>/ is committed, fuzz/corpus/ is not.
foreach(target ${BDF_FUZZ_TARGETS})
  add_executable(test_replay_${target} replay.c ../fuzz/${target}_fuzz.c)
  target_link_libraries(test_replay_${target} PRIVATE bdf::bdf)
  file(GLOB inputs CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../fuzz/regress/${target}/*)
  list(FILTER inputs EXCLUDE REGEX "/\\.gitkeep$")
  add_test(NAME replay-${target} COMMAND test_replay_${target} ${inputs})
endforeach()

# Tests built from the sources, which reach the internals: the WOFF2
# transforms on their own, the primitives behind encrypted documents, and the
# conformance runner, which reads suite.json with the library's own JSON
# reader.
set(BDF_ABS_SOURCES ${BDF_SOURCES} ${BDF_GLUE})
list(TRANSFORM BDF_ABS_SOURCES PREPEND ${PROJECT_SOURCE_DIR}/)
foreach(name woff2_internal text_internal crypto_internal conformance)
  add_executable(test_${name} ${name}.c ${BDF_ABS_SOURCES} ${BDF_BROTLI_OBJECTS})
  target_include_directories(test_${name} PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src)
  if(BDF_BROTLI_INCLUDE)
    target_include_directories(test_${name} SYSTEM PRIVATE ${BDF_BROTLI_INCLUDE})
  endif()
  target_compile_features(test_${name} PRIVATE c_std_11)
  target_compile_options(test_${name} PRIVATE ${BDF_TEST_WARNINGS})
  if(BDF_LIBM)
    target_link_libraries(test_${name} PRIVATE ${BDF_LIBM})
  endif()
endforeach()
add_test(NAME woff2-internal COMMAND test_woff2_internal)
add_test(NAME text-internal COMMAND test_text_internal)
add_test(NAME crypto-internal COMMAND test_crypto_internal)
if(BDF_CONFORMANCE_DIR)
  add_test(NAME conformance COMMAND test_conformance ${BDF_CONFORMANCE_DIR})
endif()

# The same unit tests against the single-header form, compiled with the
# library's warnings: the amalgamation must be as clean as the sources. With
# Brotli, the single header takes the decoder through BDF_HAVE_BROTLI.
if(Python3_Interpreter_FOUND)
  add_library(bdf_single STATIC single_impl.c ${BDF_SINGLE_HEADER} ${BDF_BROTLI_OBJECTS})
  target_include_directories(bdf_single PUBLIC ${BDF_SINGLE_HEADER_DIR})
  if(BDF_WITH_BROTLI)
    target_compile_definitions(bdf_single PRIVATE BDF_HAVE_BROTLI)
    target_include_directories(bdf_single SYSTEM PRIVATE ${BDF_BROTLI_INCLUDE})
  endif()
  target_compile_features(bdf_single PUBLIC c_std_11)
  target_compile_options(bdf_single PRIVATE ${BDF_WARNINGS})
  if(BDF_LIBM)
    target_link_libraries(bdf_single PUBLIC ${BDF_LIBM})
  endif()
  add_dependencies(bdf_single bdf_single_header)
  foreach(name ${BDF_TESTS})
    add_executable(test_${name}_single test_${name}.c)
    target_link_libraries(test_${name}_single PRIVATE bdf_single)
    target_compile_options(test_${name}_single PRIVATE ${BDF_TEST_WARNINGS})
    add_test(NAME ${name}-single COMMAND test_${name}_single)
  endforeach()
endif()
