# The adapters: the pages of a document drawn by another 2D library through
# bdf_play_page (include/bdf/bdf_<name>.h says how each is used). Each is a
# static library of its own, bdf::<name>, built when its option is on, and
# tested by a program that compares what it draws with what the rasterizer
# draws (tools/compare.h).

set(BDF_ADAPTER_TARGETS "")

# The adapters in C++ (Qt, Skia) are compiled with the warnings of C++.
if(MSVC)
  set(BDF_CXX_WARNINGS /W4 $<$<BOOL:${BDF_WERROR}>:/WX>)
else()
  set(BDF_CXX_WARNINGS -Wall -Wextra -Wpedantic -Wshadow $<$<BOOL:${BDF_WERROR}>:-Werror>)
endif()

# bdf_adapter(<name> <source>): the library bdf_<name> (bdf::<name>).
function(bdf_adapter name source)
  add_library(bdf_${name} STATIC ${source})
  add_library(bdf::${name} ALIAS bdf_${name})
  target_link_libraries(bdf_${name} PUBLIC bdf)
  if(source MATCHES "\\.cpp$")
    target_compile_options(bdf_${name} PRIVATE ${BDF_CXX_WARNINGS})
    target_compile_features(bdf_${name} PUBLIC cxx_std_17)
  else()
    target_compile_options(bdf_${name} PRIVATE ${BDF_WARNINGS})
  endif()
  set_target_properties(bdf_${name} PROPERTIES
    EXPORT_NAME ${name}
    POSITION_INDEPENDENT_CODE ON
    PUBLIC_HEADER ${PROJECT_SOURCE_DIR}/include/bdf/bdf_${name}.h)
  set(BDF_ADAPTER_TARGETS ${BDF_ADAPTER_TARGETS} bdf_${name} PARENT_SCOPE)
endfunction()

# bdf_adapter_test(<name> <source>): the comparison program of an adapter,
# which compares the pages built into it when it is given no document.
function(bdf_adapter_test name source)
  if(NOT BDF_BUILD_TESTS)
    return()
  endif()
  add_executable(compare_${name} ${source} tools/compare.c)
  target_include_directories(compare_${name} PRIVATE ${PROJECT_SOURCE_DIR}/tests)
  target_link_libraries(compare_${name} PRIVATE bdf_${name})
  if(BDF_LIBM)
    target_link_libraries(compare_${name} PRIVATE ${BDF_LIBM})
  endif()
  add_test(NAME adapter-${name} COMMAND compare_${name})
endfunction()

if(BDF_CAIRO)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(BDF_CAIRO_DEP REQUIRED IMPORTED_TARGET cairo)
  bdf_adapter(cairo cairo/bdf_cairo.c)
  target_link_libraries(bdf_cairo PUBLIC PkgConfig::BDF_CAIRO_DEP)
  if(BDF_LIBM)
    target_link_libraries(bdf_cairo PRIVATE ${BDF_LIBM})
  endif()
  bdf_adapter_test(cairo cairo/compare_cairo.c)
endif()

if(BDF_COREGRAPHICS)
  if(NOT APPLE)
    message(FATAL_ERROR "BDF_COREGRAPHICS needs an Apple platform")
  endif()
  bdf_adapter(coregraphics coregraphics/bdf_coregraphics.c)
  target_link_libraries(bdf_coregraphics PUBLIC "-framework CoreGraphics" "-framework CoreFoundation")
  bdf_adapter_test(coregraphics coregraphics/compare_coregraphics.c)
endif()

if(BDF_QT)
  enable_language(CXX)
  # Qt 6, or Qt 5.15 where there is no Qt 6
  find_package(Qt6 QUIET COMPONENTS Gui)
  if(Qt6_FOUND)
    set(BDF_QT_PACKAGE Qt6)
  else()
    find_package(Qt5 5.15 REQUIRED COMPONENTS Gui)
    set(BDF_QT_PACKAGE Qt5)
  endif()
  set(BDF_QT_PACKAGE ${BDF_QT_PACKAGE} PARENT_SCOPE)
  bdf_adapter(qt qt/bdf_qt.cpp)
  target_link_libraries(bdf_qt PUBLIC ${BDF_QT_PACKAGE}::Gui)
  bdf_adapter_test(qt qt/compare_qt.cpp)
endif()

if(BDF_SKIA)
  enable_language(CXX)
  # Skia has no package of its own and is built in many ways. BDF_SKIA_DIR is
  # its root: its headers are included as Skia's own are ("include/core/..."),
  # and its library is looked for under out/ there, unless BDF_SKIA_LIBRARY
  # names it. BDF_SKIA_LINK is what that library needs to be linked with,
  # when it is not what a build without its own codecs and fonts needs.
  set(BDF_SKIA_DIR "" CACHE PATH "The root of Skia: its include/ and, under out/, its library")
  set(BDF_SKIA_LIBRARY "" CACHE FILEPATH "The library of Skia, when it is not under BDF_SKIA_DIR/out")
  set(BDF_SKIA_LINK "" CACHE STRING "What the library of Skia is linked with (a list)")
  if(NOT BDF_SKIA_DIR OR NOT EXISTS ${BDF_SKIA_DIR}/include/core/SkCanvas.h)
    message(FATAL_ERROR "BDF_SKIA needs BDF_SKIA_DIR, the root of Skia (with include/core/SkCanvas.h)")
  endif()
  if(NOT BDF_SKIA_LIBRARY)
    file(GLOB_RECURSE BDF_SKIA_FOUND ${BDF_SKIA_DIR}/out/libskia.a ${BDF_SKIA_DIR}/out/skia.lib
      ${BDF_SKIA_DIR}/out/libskia.so ${BDF_SKIA_DIR}/out/libskia.dylib)
    if(NOT BDF_SKIA_FOUND)
      message(FATAL_ERROR "no library of Skia under ${BDF_SKIA_DIR}/out: set BDF_SKIA_LIBRARY")
    endif()
    list(GET BDF_SKIA_FOUND 0 BDF_SKIA_LIBRARY)
  endif()
  if(NOT BDF_SKIA_LINK)
    if(APPLE)
      set(BDF_SKIA_LINK "-framework CoreFoundation" "-framework CoreGraphics" "-framework CoreText"
        "-framework CoreServices")
    elseif(UNIX)
      set(BDF_SKIA_LINK fontconfig freetype pthread dl)
    endif()
  endif()
  message(STATUS "Skia: ${BDF_SKIA_LIBRARY}")
  bdf_adapter(skia skia/bdf_skia.cpp)
  # (as a system directory: the warnings of Skia's headers are Skia's)
  target_include_directories(bdf_skia SYSTEM PUBLIC ${BDF_SKIA_DIR})
  target_link_libraries(bdf_skia PUBLIC ${BDF_SKIA_LIBRARY} ${BDF_SKIA_LINK})
  bdf_adapter_test(skia skia/compare_skia.cpp)
endif()

set(BDF_ADAPTER_TARGETS ${BDF_ADAPTER_TARGETS} PARENT_SCOPE)
