cmake_minimum_required(VERSION 3.21)

project(bdf
  VERSION 0.8.0
  DESCRIPTION "A C reader for BDF (Browser-specific Document Format) documents"
  HOMEPAGE_URL "https://github.com/shibukawa/libbdf"
  LANGUAGES C)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(BDF_BUILD_TESTS "Build the tests" ${PROJECT_IS_TOP_LEVEL})
option(BDF_BUILD_FUZZ "Build the libFuzzer harnesses (clang only)" OFF)
option(BDF_WERROR "Treat warnings as errors" OFF)
option(BDF_INSTALL "Generate the install rules" ${PROJECT_IS_TOP_LEVEL})
option(BDF_WITH_BROTLI "Decode WOFF2 fonts with the vendored Brotli decoder (third_party/brotli)" ON)
option(BDF_CAIRO "Build bdf::cairo, which draws documents with cairo" OFF)
option(BDF_COREGRAPHICS "Build bdf::coregraphics, which draws documents with Core Graphics" OFF)
option(BDF_QT "Build bdf::qt, which draws documents with QPainter (C++)" OFF)
option(BDF_SKIA "Build bdf::skia, which draws documents with Skia (C++; see BDF_SKIA_DIR)" OFF)
set(BDF_SANITIZE "" CACHE STRING "Sanitizers to build everything with, e.g. address,undefined")
set(BDF_CONFORMANCE_DIR "" CACHE PATH "A conformance suite written by `bdf conformance`, run by ctest")

if(BDF_SANITIZE)
  add_compile_options(-fsanitize=${BDF_SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${BDF_SANITIZE})
endif()

# The sources, in the order tools/amalgamate.py joins them. The Brotli glue
# is chosen by BDF_WITH_BROTLI; the single-header form takes it from
# BDF_HAVE_BROTLI.
set(BDF_SOURCES
  src/reader.c
  src/object.c
  src/container.c
  src/inflate.c
  src/json.c
  src/manifest.c
  src/doc.c
  src/woff2.c
  src/text.c
  src/index.c
  src/unicode.c
  src/search.c
  src/crypto.c
  src/rmath.c
  src/rpath.c
  src/rscan.c
  src/rstroke.c
  src/rsurface.c
  src/rpaint.c
  src/img_png.c
  src/img_jpeg.c
  src/img_gif.c
  src/img_bmp.c
  src/img_vp8.c
  src/img_vp8l.c
  src/img_webp.c
  src/rimage.c
  src/sfnt.c
  src/fontdb.c
  src/rtext.c
  src/svg_str.c
  src/svg_float.c
  src/svg_value.c
  src/svg_xml.c
  src/svg_css.c
  src/svg_doc.c
  src/rsvg.c
  src/rcanvas.c
  src/rdraw.c
  src/render.c
  src/version.c)

if(BDF_WITH_BROTLI)
  set(BDF_GLUE src/brotli_glue.c)
  file(GLOB BDF_BROTLI_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/brotli/common/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/brotli/dec/*.c)
  set(BDF_BROTLI_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/brotli/include)
  # Compiled apart, with its own (default) warnings, into objects the library
  # takes in; its symbols stay hidden.
  add_library(bdf_brotli OBJECT ${BDF_BROTLI_SOURCES})
  target_include_directories(bdf_brotli PRIVATE ${BDF_BROTLI_INCLUDE})
  set_target_properties(bdf_brotli PROPERTIES
    C_VISIBILITY_PRESET hidden
    POSITION_INDEPENDENT_CODE ON)
  set(BDF_BROTLI_OBJECTS $<TARGET_OBJECTS:bdf_brotli>)
else()
  set(BDF_GLUE src/brotli_none.c)
  set(BDF_BROTLI_INCLUDE "")
  set(BDF_BROTLI_OBJECTS "")
endif()

add_library(bdf ${BDF_SOURCES} ${BDF_GLUE} ${BDF_BROTLI_OBJECTS})
add_library(bdf::bdf ALIAS bdf)

target_include_directories(bdf PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
if(BDF_BROTLI_INCLUDE)
  # SYSTEM: Brotli's headers declare array parameters with sizes, which gcc's
  # -Wvla takes for variable length arrays; they are not ours to warn about.
  target_include_directories(bdf SYSTEM PRIVATE ${BDF_BROTLI_INCLUDE})
endif()
target_compile_features(bdf PUBLIC c_std_11)
# The rasterizer takes square roots and remainders: the math library, where
# it is one of its own.
find_library(BDF_LIBM m)
set(BDF_PC_LIBS_PRIVATE "")
if(BDF_LIBM)
  target_link_libraries(bdf PRIVATE ${BDF_LIBM})
  set(BDF_PC_LIBS_PRIVATE "-lm")
endif()
# Its arithmetic must not be fused (src/raster_internal.h says why, and
# turns it off itself where a compiler has a pragma for that).
if(NOT MSVC)
  include(CheckCCompilerFlag)
  check_c_compiler_flag(-ffp-contract=off BDF_HAS_FP_CONTRACT_OFF)
  if(BDF_HAS_FP_CONTRACT_OFF)
    target_compile_options(bdf PRIVATE -ffp-contract=off)
  endif()
endif()
set_target_properties(bdf PROPERTIES
  C_VISIBILITY_PRESET hidden
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  OUTPUT_NAME bdf
  PUBLIC_HEADER include/bdf/bdf.h)
target_compile_definitions(bdf PRIVATE BDF_BUILDING)
if(BUILD_SHARED_LIBS)
  target_compile_definitions(bdf PUBLIC BDF_SHARED)
endif()

if(MSVC)
  set(BDF_WARNINGS /W4 $<$<BOOL:${BDF_WERROR}>:/WX>)
else()
  set(BDF_WARNINGS
    -Wall -Wextra -Wpedantic -Wshadow -Wvla -Wcast-qual -Wstrict-prototypes
    $<$<BOOL:${BDF_WERROR}>:-Werror>)
endif()
target_compile_options(bdf PRIVATE ${BDF_WARNINGS})

# The single-header form: generated from the same sources, and tested
# alongside them when Python is at hand.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  set(BDF_SINGLE_HEADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/single)
  # the headers of src/, which the sources bring into the single header
  file(GLOB BDF_INTERNAL_HEADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h)
  set(BDF_SINGLE_HEADER ${BDF_SINGLE_HEADER_DIR}/bdf/bdf.h)
  add_custom_command(OUTPUT ${BDF_SINGLE_HEADER}
    COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/amalgamate.py -o ${BDF_SINGLE_HEADER}
    DEPENDS ${BDF_SOURCES} ${BDF_INTERNAL_HEADERS} src/brotli_glue.c src/brotli_none.c include/bdf/bdf.h tools/amalgamate.py
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating the single-header bdf.h"
    VERBATIM)
  add_custom_target(bdf_single_header DEPENDS ${BDF_SINGLE_HEADER})
else()
  message(STATUS "Python 3 not found: the single-header form is not generated or tested")
endif()

if(BDF_BUILD_FUZZ)
  # The library itself carries the coverage instrumentation the fuzzer steers by.
  target_compile_options(bdf PRIVATE -fsanitize=fuzzer-no-link)
  # Objects that draw each other again and again are stopped by a limit that
  # takes seconds to reach; a fuzzer learns as much from a lower one.
  target_compile_definitions(bdf PRIVATE BDF_MAX_REUSED_INSTRUCTIONS=65536)
  # A document may ask for ten million iterations of its key derivation:
  # seconds for each input.
  target_compile_definitions(bdf PRIVATE BDF_MAX_KDF_ITERATIONS=2048)
  # So do objects drawn again and again by a renderer, and shadows blurred
  # over a thousand pixels.
  target_compile_definitions(bdf PRIVATE BDF_MAX_REUSED_DRAWN=65536 BDF_MAX_SHADOW_BLUR=16)
  # And images of a hundred megapixels.
  target_compile_definitions(bdf PRIVATE BDF_MAX_DECODE_PIXELS=1048576)
  # And SVG images drawn large, of hundreds of thousands of elements.
  target_compile_definitions(bdf PRIVATE BDF_MAX_SVG_RASTER_PIXELS=65536 BDF_MAX_SVG_ELEMENTS=2000)
  add_subdirectory(fuzz)
endif()

if(BDF_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

add_subdirectory(adapters)

if(BDF_INSTALL)
  install(TARGETS bdf ${BDF_ADAPTER_TARGETS} EXPORT bdfTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/bdf)
  install(EXPORT bdfTargets
    NAMESPACE bdf::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/bdf)
  configure_package_config_file(cmake/bdfConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/bdfConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/bdf)
  write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/bdfConfigVersion.cmake
    COMPATIBILITY SameMajorVersion)
  install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/bdfConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/bdfConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/bdf)
  # The prefix of the pkg-config files is where they are found, so that an
  # installed tree can be moved (and installed with --prefix).
  # (from a place that is a full path on every system: only what is
  # relative of it is kept)
  set(BDF_PC_ROOT "${CMAKE_CURRENT_BINARY_DIR}/prefix")
  file(RELATIVE_PATH BDF_PC_PREFIX "${BDF_PC_ROOT}/${CMAKE_INSTALL_LIBDIR}/pkgconfig" "${BDF_PC_ROOT}")
  string(REGEX REPLACE "/+$" "" BDF_PC_PREFIX "${BDF_PC_PREFIX}")
  configure_file(cmake/bdf.pc.in ${CMAKE_CURRENT_BINARY_DIR}/bdf.pc @ONLY)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/bdf.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
  # An adapter that pkg-config can name what it needs for has a file of its own.
  if(BDF_CAIRO)
    set(BDF_ADAPTER_NAME cairo)
    set(BDF_ADAPTER_REQUIRES cairo)
    configure_file(cmake/bdf-adapter.pc.in ${CMAKE_CURRENT_BINARY_DIR}/bdf-cairo.pc @ONLY)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/bdf-cairo.pc
      DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
  endif()
endif()
