set(BDF_TESTS object header inflate manifest doc)
set(BDF_FUZZ_TARGETS object inflate manifest doc)

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

# The same unit tests against the single-header form, compiled with the
# library's warnings: the amalgamation must be as clean as the sources.
if(Python3_Interpreter_FOUND)
  add_library(bdf_single STATIC single_impl.c ${BDF_SINGLE_HEADER})
  target_include_directories(bdf_single PUBLIC ${BDF_SINGLE_HEADER_DIR})
  target_compile_features(bdf_single PUBLIC c_std_11)
  target_compile_options(bdf_single PRIVATE ${BDF_WARNINGS})
  add_dependencies(bdf_single bdf_single_header)
  foreach(name ${BDF_TESTS})
    add_executable(test_${name}_single test_${name}.c)
    target_link_libraries(test_${name}_single PRIVATE bdf_single)
    target_compile_options(test_${name}_single PRIVATE ${BDF_TEST_WARNINGS})
    add_test(NAME ${name}-single COMMAND test_${name}_single)
  endforeach()
endif()

# The conformance suite of the reference implementation (bdf conformance),
# when one is at hand. The runner reads suite.json with the library's own
# JSON reader, so it is built from the sources rather than linked.
set(BDF_ABS_SOURCES ${BDF_SOURCES})
list(TRANSFORM BDF_ABS_SOURCES PREPEND ${PROJECT_SOURCE_DIR}/)
add_executable(test_conformance conformance.c ${BDF_ABS_SOURCES})
target_include_directories(test_conformance PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src)
target_compile_features(test_conformance PRIVATE c_std_11)
target_compile_options(test_conformance PRIVATE ${BDF_TEST_WARNINGS})
if(BDF_CONFORMANCE_DIR)
  add_test(NAME conformance COMMAND test_conformance ${BDF_CONFORMANCE_DIR})
endif()
